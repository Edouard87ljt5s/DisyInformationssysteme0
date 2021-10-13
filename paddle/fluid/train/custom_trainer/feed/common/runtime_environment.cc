#include <mpi.h>
#include "paddle/fluid/train/custom_trainer/feed/common/runtime_environment.h"

namespace paddle {
namespace custom_trainer {
namespace feed {

template<class T>
struct mpi_type_trait {
};
template<>
struct mpi_type_trait<double> {
    static MPI_Datatype type() {
        return MPI_DOUBLE;
    }
};
template<>
struct mpi_type_trait<float> {
    static MPI_Datatype type() {
        return MPI_FLOAT;
    }
};
template<>
struct mpi_type_trait<int32_t> {
    static MPI_Datatype type() {
        return MPI_INT;
    }
};
template<>
struct mpi_type_trait<uint32_t> {
    static MPI_Datatype type() {
        return MPI_UNSIGNED;
    }
};
template<>
struct mpi_type_trait<int64_t> {
    static MPI_Datatype type() {
        return MPI_LONG_LONG;
    }
};
template<>
struct mpi_type_trait<uint64_t> {
    static MPI_Datatype type() {
        return MPI_UNSIGNED_LONG_LONG;
    }
};
template<>
struct mpi_type_trait<long long> {
    static MPI_Datatype type() {
        return MPI_LONG_LONG;
    }
};
template<>
struct mpi_type_trait<unsigned long long> {
    static MPI_Datatype type() {
        return MPI_UNSIGNED_LONG_LONG;
    }
};

RuntimeEnvironment::RuntimeEnvironment() {}
RuntimeEnvironment::~RuntimeEnvironment() {}
bool RuntimeEnvironment::is_master_node(EnvironmentRole role) {
    return rank_id(role) == 0;
}
std::string format_timestamp(time_t time, const char* format) {
    std::string result;
    struct tm p = *localtime(&time);
    char time_str_buffer[64];
    int size = strftime (time_str_buffer, 64, format, &p);
    if (size > 0) {
        result.assign(time_str_buffer, size);
    }
    return result;
}

struct MpiNodeInfo {
    int rank_id = -1;
    int node_num = 0;
    MPI_Comm mpi_comm;
};

class MPIRuntimeEnvironment : public RuntimeEnvironment {
public:
    MPIRuntimeEnvironment() {}
    virtual ~MPIRuntimeEnvironment() {}
    virtual int initialize(YAML::Node config) {
        return 0;
    }
    virtual int wireup() {
        int hr = MPI_Init(NULL, NULL);
        if (MPI_SUCCESS != hr) {
            LOG(FATAL) << "MPI_init failed with error code" << hr; 
            return -1;
        }
        _roles_node_info.resize(static_cast<int>(EnvironmentRole::ALL) + 1);
        set_role(EnvironmentRole::ALL);
        return 0;
    }

    virtual uint32_t rank_id(EnvironmentRole role) {
        return mpi_node_info(role).rank_id;
    }
    virtual uint32_t node_num(EnvironmentRole role) {
        return mpi_node_info(role).node_num;
    }
    virtual int set_role(EnvironmentRole role) {
        auto& node_info = mpi_node_info(role);
        if (node_info.rank_id < 0) {
            if (role == EnvironmentRole::ALL) {
                node_info.mpi_comm = MPI_COMM_WORLD;
            } else {
                MPI_Comm_split(MPI_COMM_WORLD, static_cast<int>(role), 
                    mpi_node_info(EnvironmentRole::ALL).rank_id, &(node_info.mpi_comm));
            }
            MPI_Comm_rank(node_info.mpi_comm, &(node_info.rank_id));
            MPI_Comm_size(node_info.mpi_comm, &(node_info.node_num));
        }
        return 0;
    }

    virtual void barrier(EnvironmentRole role) {
        MPI_Barrier(mpi_node_info(role).mpi_comm);
    }

    virtual void bcast(paddle::framework::BinaryArchive& ar, int root_id, EnvironmentRole role) {
        auto& node_info = mpi_node_info(role);
        int len = (int)ar.Length();
        MPI_Bcast(&len, 1, MPI_INT, root_id, node_info.mpi_comm);
        ar.Resize(len);
        ar.SetCursor(ar.Buffer());
        MPI_Bcast(ar.Buffer(), len, MPI_BYTE, root_id, node_info.mpi_comm);
    }

    virtual double all_reduce_ele(double x) {
        double tot = 0.0;
        MPI_Allreduce(&x, &tot, 1, mpi_type_trait<double>::type(), MPI_SUM, MPI_COMM_WORLD);
        return tot;
    }
    virtual void all_reduce_arr(double* x, int n) {
        MPI_Allreduce(MPI_IN_PLACE, x, n, mpi_type_trait<double>::type(), MPI_SUM, MPI_COMM_WORLD);
    }

protected:
    virtual void print_log(EnvironmentRole role, EnvironmentLogType type, 
        EnvironmentLogLevel level,  const std::string& log_str) {
        if (type == EnvironmentLogType::MASTER_LOG && !is_master_node(role)) {
            return;
        }
        VLOG(static_cast<int>(level)) << log_str;
    }

    inline MpiNodeInfo& mpi_node_info(EnvironmentRole role) {
        return _roles_node_info[static_cast<int>(role)];
    }

private:
    std::vector<MpiNodeInfo> _roles_node_info;
};
REGISTER_CLASS(RuntimeEnvironment, MPIRuntimeEnvironment);

//用于本地模式单机训练
class LocalRuntimeEnvironment : public RuntimeEnvironment {
public:
    LocalRuntimeEnvironment() {}
    virtual ~LocalRuntimeEnvironment() {}
    virtual int initialize(YAML::Node config) {
        return 0;
    }
    virtual int wireup() {
        return 0;
    }
    virtual uint32_t rank_id(EnvironmentRole role) {
        return 0;
    }
    virtual uint32_t node_num(EnvironmentRole role) {
        return 1;
    }
    virtual int set_role(EnvironmentRole role) {
        return 0;
    }
    virtual void barrier(EnvironmentRole role) {
        return;
    }
    virtual void bcast(paddle::framework::BinaryArchive& ar, int root_id, EnvironmentRole role) {
        return;
    }
    virtual double all_reduce_ele(double x) {
        return x;
    }
    virtual void all_reduce_arr(double* x, int n) {
        return;
    }
protected:
    virtual void print_log(EnvironmentRole role, EnvironmentLogType type, 
        EnvironmentLogLevel level,  const std::string& log_str) {
        VLOG(static_cast<int>(level)) << log_str;
    }
};
REGISTER_CLASS(RuntimeEnvironment, LocalRuntimeEnvironment);

}  // namespace feed
}  // namespace custom_trainer
}  // namespace paddle
