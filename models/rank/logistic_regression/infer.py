# Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import paddle
import os
import paddle.nn as nn
import lr_net as net
import time
import logging

from utils import load_yaml, get_abs_model, save_model, load_model
from criteo_lr_reader_dygraph import CriteoLRDataset
from paddle.io import DistributedBatchSampler, DataLoader
import argparse

logging.basicConfig(
    format='%(asctime)s - %(levelname)s - %(message)s', level=logging.INFO)
logger = logging.getLogger(__name__)


def parse_args():
    parser = argparse.ArgumentParser(description='paddle-rec run')
    parser.add_argument("-m", "--config_yaml", type=str)
    args = parser.parse_args()
    args.config_yaml = get_abs_model(args.config_yaml)
    return args


def create_feeds(batch, num_field):
    label = paddle.to_tensor(batch[0].numpy().astype('int32').reshape(-1, 1))
    feat_idx = paddle.to_tensor(batch[1].numpy().astype('int64').reshape(-1,
                                                                         1))
    raw_feat_value = paddle.to_tensor(batch[2].numpy().astype('float32')
                                      .reshape(-1, 1))
    feat_value = paddle.reshape(raw_feat_value,
                                [-1, num_field])  # None * num_field * 1
    return label, feat_idx, feat_value


def create_model(config):
    init_value = 0.1
    sparse_feature_number = config.get(
        'hyper_parameters.sparse_feature_number', None)
    reg = config.get('hyper_parameters.reg', None)
    num_field = config.get('hyper_parameters.num_field', None)

    LR = net.LRLayer(sparse_feature_number, init_value, reg, num_field)

    return LR


def create_data_loader(dataset, mode, place, config):
    batch_size = config.get('dygraph.batch_size', None)
    is_train = mode == 'train'
    batch_sampler = DistributedBatchSampler(
        dataset, batch_size=batch_size, shuffle=is_train)
    loader = DataLoader(dataset, batch_sampler=batch_sampler, places=place)
    return loader


def main(args):
    paddle.seed(12345)
    config = load_yaml(args.config_yaml)
    use_gpu = config.get("dygraph.use_gpu", True)
    test_data_dir = config.get("dygraph.test_data_dir", None)
    feature_size = config.get('hyper_parameters.feature_size', None)
    print_interval = config.get("dygraph.print_interval", None)
    model_load_path = config.get("dygraph.infer_load_path", "model_output")
    start_epoch = config.get("dygraph.infer_start_epoch", -1)
    end_epoch = config.get("dygraph.infer_end_epoch", 10)
    num_field = config.get('hyper_parameters.num_field', None)

    place = paddle.set_device('gpu' if use_gpu else 'cpu')

    print("***********************************")
    logger.info(
        "use_gpu: {}, test_data_dir: {}, start_epoch: {}, end_epoch: {}, print_interval: {}, model_load_path: {}".
        format(use_gpu, test_data_dir, start_epoch, end_epoch, print_interval,
               model_load_path))
    print("***********************************")

    lr_model = create_model(config)
    file_list = [
        os.path.join(test_data_dir, x) for x in os.listdir(test_data_dir)
    ]
    print("read data")
    dataset = CriteoLRDataset(file_list)
    test_dataloader = create_data_loader(
        dataset, mode='test', place=place, config=config)

    auc_metric = paddle.metric.Auc("ROC")
    epoch_begin = time.time()
    interval_begin = time.time()

    for epoch_id in range(start_epoch + 1, end_epoch):

        logger.info("load model epoch {}".format(epoch_id))
        model_path = os.path.join(model_load_path, str(epoch_id))
        load_model(model_path, lr_model)
        for batch_id, batch in enumerate(test_dataloader()):
            batch_size = len(batch[0])

            label, feat_idx, feat_value = create_feeds(batch, num_field)

            pred = lr_model(feat_idx, feat_value)

            # for auc
            predict_2d = paddle.concat(x=[1 - pred, pred], axis=1)
            label_int = paddle.cast(label, 'int64')
            auc_metric.update(
                preds=predict_2d.numpy(), labels=label_int.numpy())

            if batch_id % print_interval == 1:
                logger.info(
                    "infer epoch: {}, batch_id: {}, auc: {:.6f}, speed: {:.2f} ins/s".
                    format(epoch_id, batch_id,
                           auc_metric.accumulate(), print_interval * batch_size
                           / (time.time() - interval_begin)))
                interval_begin = time.time()

        logger.info(
            "infer epoch: {} done, auc: {:.6f}, : epoch time{:.2f} s".format(
                epoch_id, auc_metric.accumulate(), time.time() - epoch_begin))


if __name__ == '__main__':
    args = parse_args()
    main(args)
