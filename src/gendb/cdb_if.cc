/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "cdb_if.h"
#include <boost/bind.hpp>
#include <boost/cast.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/assign/list_of.hpp>

#include "base/parse_object.h"

#include <sandesh/sandesh_constants.h>
#include <sandesh/sandesh_types.h>
#include <sandesh/sandesh.h>
#include "analytics_db_types.h"

using boost::lexical_cast;
namespace cassandra = ::org::apache::cassandra;

/*
 * Types supported by Cassandra are the following, but we use only a subset for
 * now
    AsciiType = 1,
    LongType = 2,
    BytesType = 3,
    BooleanType = 4,
    CounterColumnType = 5,
    DecimalType = 6,
    DoubleType = 7,
    FloatType = 8,
    Int32Type = 9,
    UTF8Type = 10,
    DateType = 11,
    LexicalUUIDType = 12,
    IntegerType = 13,
    TimeUUIDType = 14,
    CompositeType = 15,
 */

CdbIf::CdbIfTypeMapDef CdbIf::CdbIfTypeMap =
    boost::assign::map_list_of
        (GenDb::DbDataType::AsciiType, CdbIf::CdbIfTypeInfo("AsciiType",
                                                     CdbIf::Db_encode_string_composite,
                                                     CdbIf::Db_decode_string_composite,
                                                     CdbIf::Db_encode_string_non_composite,
                                                     CdbIf::Db_decode_string_non_composite))
        (GenDb::DbDataType::LexicalUUIDType, CdbIf::CdbIfTypeInfo("LexicalUUIDType",
                                                     CdbIf::Db_encode_UUID_composite,
                                                     CdbIf::Db_decode_UUID_composite,
                                                     CdbIf::Db_encode_UUID_non_composite,
                                                     CdbIf::Db_decode_UUID_non_composite))
        (GenDb::DbDataType::TimeUUIDType, CdbIf::CdbIfTypeInfo("TimeUUIDType",
                                                     CdbIf::Db_encode_UUID_composite,
                                                     CdbIf::Db_decode_UUID_composite,
                                                     CdbIf::Db_encode_UUID_non_composite,
                                                     CdbIf::Db_decode_UUID_non_composite))
        (GenDb::DbDataType::Unsigned8Type, CdbIf::CdbIfTypeInfo("IntegerType",
                                                     CdbIf::Db_encode_Unsigned8_composite,
                                                     CdbIf::Db_decode_Unsigned8_composite,
                                                     CdbIf::Db_encode_Unsigned8_non_composite,
                                                     CdbIf::Db_decode_Unsigned8_non_composite))
        (GenDb::DbDataType::Unsigned16Type, CdbIf::CdbIfTypeInfo("IntegerType",
                                                     CdbIf::Db_encode_Unsigned16_composite,
                                                     CdbIf::Db_decode_Unsigned16_composite,
                                                     CdbIf::Db_encode_Unsigned16_non_composite,
                                                     CdbIf::Db_decode_Unsigned16_non_composite))
        (GenDb::DbDataType::Unsigned32Type, CdbIf::CdbIfTypeInfo("IntegerType",
                                                     CdbIf::Db_encode_Unsigned32_composite,
                                                     CdbIf::Db_decode_Unsigned32_composite,
                                                     CdbIf::Db_encode_Unsigned32_non_composite,
                                                     CdbIf::Db_decode_Unsigned32_non_composite))
        (GenDb::DbDataType::Unsigned64Type, CdbIf::CdbIfTypeInfo("IntegerType",
                                                     CdbIf::Db_encode_Unsigned64_composite,
                                                     CdbIf::Db_decode_Unsigned64_composite,
                                                     CdbIf::Db_encode_Unsigned64_non_composite,
                                                     CdbIf::Db_decode_Unsigned64_non_composite))
        ;

CdbIf::~CdbIf() { 
    transport_->close();
    TimerManager::DeleteTimer(periodic_timer_);
    periodic_timer_ = NULL;
}

CdbIf::CdbIf(boost::asio::io_service *ioservice, DbErrorHandler errhandler,
        std::string cassandra_ip, unsigned short cassandra_port, bool enable_stats, int ttl) :
    socket_(new TSocket(cassandra_ip, cassandra_port)),
    transport_(new TFramedTransport(socket_)),
    protocol_(new TBinaryProtocol(transport_)),
    client_(new CassandraClient(protocol_)),
    ioservice_(ioservice),
    errhandler_(errhandler),
    db_init_done_(false),
    periodic_timer_(TimerManager::CreateTimer(*ioservice, "Cdb Periodic timer")),
    enable_stats_(enable_stats),
    cassandra_ttl_(ttl) {

    boost::system::error_code error;
    name_ = boost::asio::ip::host_name(error);
}

bool CdbIf::Db_IsInitDone() {
    return db_init_done_;
}

void CdbIf::Db_SetInitDone(bool init_done) {
    if (db_init_done_ != init_done) {
        if (init_done) {
            // Start cdbq dequeue if init is done
            cdbq_->MayBeStartRunner();
        }
        db_init_done_ = init_done;
    }
}

bool CdbIf::Db_Init(std::string task_id, int task_instance) {
    /*
     * we can leave the queue contents as is so they can be replayed after the
     * connection to db is established
     */
    if (!cdbq_.get()) {
        cdbq_.reset(new WorkQueue<CdbIfColList *>(
            TaskScheduler::GetInstance()->GetTaskId(task_id), task_instance,
            boost::bind(&CdbIf::Db_AsyncAddColumn, this, _1),
            boost::bind(&CdbIf::Db_IsInitDone, this)));
    }

    if (enable_stats_) {
        periodic_timer_->Start(PeriodicTimeSec * 1000,
                boost::bind(&CdbIf::PeriodicTimerExpired, this),
                boost::bind(&CdbIf::PeriodicTimerErrorHandler, this, _1, _2));
    }

    try {
        transport_->open();
    } catch (TTransportException &tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TTransportException what: " << tx.what());
    } catch (TException &tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TException what: " << tx.what());
    }

    return true;
}

void CdbIf::Db_Uninit(bool shutdown) {
    try {
        transport_->close();
    } catch (TTransportException &tx) {
        CDBIF_HANDLE_EXCEPTION(__func__ << ": TTransportException what: " << tx.what());
    } catch (TException &tx) {
        CDBIF_HANDLE_EXCEPTION(__func__ << ": TException what: " << tx.what());
    }
    if (enable_stats_) {
        periodic_timer_->Cancel();
    }
    if (shutdown) {
        cdbq_->Shutdown();
        cdbq_.reset();
    }
}

bool CdbIf::Db_AddTablespace(const std::string& tablespace) {
    if (!Db_FindTablespace(tablespace)) {
        KsDef ks_def;
        ks_def.__set_name(tablespace);
        ks_def.__set_strategy_class("SimpleStrategy");
        std::map<std::string, std::string> strat_options;
        strat_options.insert(std::pair<std::string, std::string>("replication_factor", "1"));
        ks_def.__set_strategy_options(strat_options);

        try {
            std::string retval;
            client_->system_add_keyspace(retval, ks_def);
        } catch (SchemaDisagreementException &tx) {
            CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": SchemaDisagreementException: " << tx.what());
        } catch (InvalidRequestException &tx) {
            CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": InvalidRequestException: " << tx.why);
        } catch (TApplicationException &tx) {
            CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TApplicationException: " << tx.what());
        } catch (TException &tx) {
            CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TException what: " << tx.what());
        }
        return true;
    }

    return true;
}

bool CdbIf::Db_SetTablespace(const std::string& tablespace) {
    if (!Db_FindTablespace(tablespace)) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": tablespace not found");
    }

    try {
        client_->set_keyspace(tablespace);
        tablespace_ = tablespace;
    } catch (InvalidRequestException &tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": InvalidRequestException: " << tx.why);
    } catch (TException &tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TException what: " << tx.what());
    }

    KsDef retval;
    try {
        client_->describe_keyspace(retval, tablespace);
    } catch (NotFoundException &tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": NotFoundException: " << tx.what());
    } catch (InvalidRequestException &tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": InvalidRequestException: " << tx.why);
    } catch (TApplicationException &tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TApplicationException: " << tx.what());
    } catch (TException &tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TException what: " << tx.what());
    }

    std::vector<CfDef>::const_iterator iter;
    for (iter = retval.cf_defs.begin(); iter != retval.cf_defs.end(); iter++) {
        std::string name = (*iter).name;
        CfDef *cfdef = new CfDef;
        *cfdef = *iter;
        CdbIfCfList.insert(name, new CdbIfCfInfo(cfdef));
    }
    return true;
}

bool CdbIf::Db_AddSetTablespace(const std::string& tablespace) {
    if (!Db_AddTablespace(tablespace)) {
        return false;
    }
    if (!Db_SetTablespace(tablespace)) {
        return false;
    }
    return true;
}

bool CdbIf::Db_FindTablespace(const std::string& tablespace) {
    try {
        KsDef retval;
        client_->describe_keyspace(retval, tablespace);
    } catch (NotFoundException &tx) {
        return false;
    } catch (InvalidRequestException &tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": InvalidRequestException: " << tx.why);
    } catch (TApplicationException &tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TApplicationException: " << tx.what());
    } catch (TException &tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TException what: " << tx.what());
    }
    return true;
}

bool CdbIf::Db_GetColumnfamily(CdbIfCfInfo **info, const std::string& cfname) {
    CdbIfCfListType::iterator it;
    if ((it = CdbIfCfList.find(cfname)) != CdbIfCfList.end()) {
        *info = it->second;
        return true;
    }

    return false;
}

bool CdbIf::Db_UseColumnfamily(const GenDb::NewCf& cf) {
    if (Db_FindColumnfamily(cf.cfname_)) {
        return true;
    }

    CdbIfCfInfo *cfinfo;
    if (!Db_GetColumnfamily(&cfinfo, cf.cfname_)) {
        return false;
    }
    cfinfo->cf_.reset(new GenDb::NewCf(cf));
    return true;
}

bool CdbIf::Db_FindColumnfamily(const std::string& cfname) {
    CdbIfCfListType::iterator it;
    if ((it = CdbIfCfList.find(cfname)) != CdbIfCfList.end() &&
            (it->second->cf_.get())) {
        return true;
    }

    return false;
}

bool CdbIf::Db_Columnfamily_present(const std::string& cfname) {
    CdbIfCfListType::iterator it;
    if ((it = CdbIfCfList.find(cfname)) != CdbIfCfList.end()) {
        return true;
    }

    return false;
}

bool CdbIf::DbDataTypeVecToCompositeType(std::string& res, const GenDb::DbDataTypeVec& db_type) {
    if (db_type.size() == 0) {
        return false;
    } else if (db_type.size() == 1) {
        CdbIfTypeMapDef::iterator it;
        if ((it = CdbIfTypeMap.find(db_type.front())) == CdbIfTypeMap.end())
            return false;

        res = it->second.cassandra_type_;
        return true;
    } else {
        res = "CompositeType(";
        std::vector<GenDb::DbDataType::type>::const_iterator it = db_type.begin();
        CdbIfTypeMapDef::iterator jt;

        if ((jt = CdbIfTypeMap.find(*it)) == CdbIfTypeMap.end())
            return false;
        res.append(jt->second.cassandra_type_);

        it++;
        for (; it != db_type.end(); it++) {
            res.append(", ");
            if ((jt = CdbIfTypeMap.find(*it)) == CdbIfTypeMap.end())
                return false;
            res.append(jt->second.cassandra_type_);
        }
        res.append(")");
        return true;
    }
}

bool CdbIf::DbDataValueFromType(GenDb::DbDataValue& res, const GenDb::DbDataType::type& type, const std::string& input) {
    CdbIfTypeMapDef::iterator it = CdbIfTypeMap.find(type);

    if (it == CdbIfTypeMap.end()) {
        return false;
    }
    res = it->second.decode_non_composite_fn_(input);

    return true;
}

bool CdbIf::DbDataValueFromString(GenDb::DbDataValue& res, const std::string& cfname,
        const std::string& col_name, const std::string& input) {
    CdbIfCfInfo *info;
    GenDb::NewCf *cf;

    if (!Db_GetColumnfamily(&info, cfname) ||
            !((cf = info->cf_.get()))) {
        return false;
    }
    NewCf::SqlColumnMap::iterator it;
    if ((it = cf->cfcolumns_.find(col_name)) == cf->cfcolumns_.end()) {
        return false;
    }
    CdbIfTypeMapDef::iterator jt = CdbIfTypeMap.find(it->second);

    if (jt == CdbIfTypeMap.end()) {
        return false;
    }
    res = jt->second.decode_non_composite_fn_(input);

    return true;
}

bool CdbIf::DbDataValueToString(std::string& res,
        const GenDb::DbDataType::type& type,
        const GenDb::DbDataValue& value) {
    CdbIfTypeMapDef::iterator it = CdbIfTypeMap.find(type);
    assert(it != CdbIfTypeMap.end());
    res = it->second.encode_non_composite_fn_(value);

    return true;
}

bool CdbIf::DbDataValueToStringFromCf(std::string& res, const std::string& cfname,
        const std::string& col_name, const GenDb::DbDataValue& value) {
    CdbIfCfInfo *info;
    GenDb::NewCf *cf;

    if (!Db_GetColumnfamily(&info, cfname) ||
            !((cf = info->cf_.get()))) {
        return false;
    }
    NewCf::SqlColumnMap::iterator it;
    if ((it = cf->cfcolumns_.find(col_name)) == cf->cfcolumns_.end()) {
        return false;
    }
    GenDb::DbDataType::type type = it->second;

    return(DbDataValueToString(res, type, value));
}

bool CdbIf::DbDataValueVecToString(std::string& res,
        const GenDb::DbDataTypeVec& typevec,
        const GenDb::DbDataValueVec& input) {
    CDBIF_CONDCHECK_LOG_RETF(typevec.size() != 0);
    if (typevec.size() == 1) {
        CDBIF_CONDCHECK_LOG_RETF(input.size() <= 1);
        if (input.size() == 1) {
            CdbIfTypeMapDef::iterator it = CdbIfTypeMap.find(typevec[0]);
            CDBIF_CONDCHECK_LOG_RETF(it != CdbIfTypeMap.end());
            res = CdbIfTypeMap.find(typevec[0])->second.encode_non_composite_fn_(input[0]);
        }
    } else {
        CdbIfTypeMapDef::iterator kt;
        GenDb::DbDataTypeVec::const_iterator it = typevec.begin();
        GenDb::DbDataValueVec::const_iterator jt = input.begin();
        for (;
                it != typevec.end() &&
                jt != input.end(); it++, jt++) {
            kt = CdbIfTypeMap.find(*it);
            CDBIF_CONDCHECK_LOG_RETF(kt != CdbIfTypeMap.end());
            res.append(kt->second.encode_composite_fn_(*jt));
        }

        if (it == typevec.end() && jt != input.end()) {
            CDBIF_CONDCHECK_LOG_RETF(0);
        }
    }

    return true;
}

bool CdbIf::DbDataValueVecFromString(GenDb::DbDataValueVec& res,
        const GenDb::DbDataTypeVec& typevec,
        const std::string& input) {
    if (typevec.size() == 1) {
        GenDb::DbDataValue res1;
        if (!CdbIf::DbDataValueFromType(res1, typevec.at(0), input)) {
            CDBIF_CONDCHECK_LOG_RETF(0);
        }
        res.push_back(res1);
    } else {
        int used = 0;
        const int str_size = input.size();
        const char *data = input.c_str();
        for (GenDb::DbDataTypeVec::const_iterator it = typevec.begin();
                it != typevec.end(); it++) {
            GenDb::DbDataValue res1;
            CdbIfTypeMapDef::iterator jt = CdbIfTypeMap.find(*it);
            if (jt == CdbIfTypeMap.end()) {
                CDBIF_CONDCHECK_LOG(0);
                continue;
            }
            int elem_use;
            CDBIF_CONDCHECK_LOG_RETF(used < str_size);
            res1 = jt->second.decode_composite_fn_(data+used, elem_use);
            used += elem_use;
            res.push_back(res1);
        }
    }
    return true;
}

bool CdbIf::ConstructDbDataValueKey(std::string& res, const std::string& cfname, const GenDb::DbDataValueVec& rowkey) {
    CdbIfCfInfo *info;
    GenDb::NewCf *cf;
    if (!Db_GetColumnfamily(&info, cfname) ||
            !(cf = info->cf_.get())) {
        LOG(ERROR,  __func__ << ": cf not found cf= " << cfname);
        return false;
    }

    return (DbDataValueVecToString(res, cf->key_validation_class, rowkey));
}

bool CdbIf::ConstructDbDataValueColumnName(std::string& res, const std::string& cfname, const GenDb::DbDataValueVec& name) {
    CdbIfCfInfo *info;
    GenDb::NewCf *cf;
    if (!Db_GetColumnfamily(&info, cfname) ||
            !(cf = info->cf_.get())) {
        LOG(ERROR,  __func__ << ": cf not found cf= " << cfname);
        return false;
    }
    return (DbDataValueVecToString(res, cf->comparator_type, name));
}

bool CdbIf::ConstructDbDataValueColumnValue(std::string& res, const std::string& cfname, const GenDb::DbDataValueVec& value) {
    CdbIfCfInfo *info;
    GenDb::NewCf *cf;
    if (!Db_GetColumnfamily(&info, cfname) ||
            !(cf = info->cf_.get())) {
        LOG(ERROR,  __func__ << ": cf not found cf= " << cfname);
        return false;
    }
    return (DbDataValueVecToString(res, cf->default_validation_class, value));
}

bool CdbIf::NewDb_AddColumnfamily(const GenDb::NewCf& cf) {
    if (Db_FindColumnfamily(cf.cfname_)) {
        return true;
    }

    if (cf.cftype_ == GenDb::NewCf::COLUMN_FAMILY_SQL) {
        cassandra::CfDef cf_def;
        std::string ret;

        cf_def.__set_keyspace(tablespace_);
        cf_def.__set_name(cf.cfname_);

        std::string key_valid_class;
        if (!DbDataTypeVecToCompositeType(key_valid_class, cf.key_validation_class))
            return false;
        cf_def.__set_key_validation_class(key_valid_class);

        cassandra::ColumnDef col_def;
        std::vector<cassandra::ColumnDef> col_vec;

        GenDb::NewCf::SqlColumnMap::const_iterator it;
        CdbIfTypeMapDef::iterator jt;
        for (it = cf.cfcolumns_.begin(); it != cf.cfcolumns_.end(); it++) {
            col_def.__set_name(it->first);
            if ((jt = CdbIfTypeMap.find(it->second)) == CdbIfTypeMap.end())
                return false;
            col_def.__set_validation_class(jt->second.cassandra_type_);
            col_vec.push_back(col_def);
        }
        cf_def.__set_column_metadata(col_vec);

        CdbIfCfInfo *cfinfo;
        if (Db_GetColumnfamily(&cfinfo, cf.cfname_)) {
            // TBD assert((*cfinfo->cfdef_.get()) == cf_def);
            cfinfo->cf_.reset(new GenDb::NewCf(cf));
        } else {
            try {
                client_->system_add_column_family(ret, cf_def);
            } catch (SchemaDisagreementException &tx) {
                CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": SchemaDisagreementException: " << tx.what());
            } catch (InvalidRequestException &tx) {
                CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": InvalidRequestException: " << tx.why);
            } catch (TApplicationException &tx) {
                CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TApplicationException: " << tx.what());
            } catch (TException &tx) {
                CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TException what: " << tx.what());
            }

            CfDef *cfdef_n = new CfDef;
            *cfdef_n = cf_def;
            std::string cfname_n = cf.cfname_;
            CdbIfCfList.insert(cfname_n, new CdbIfCfInfo(cfdef_n, new GenDb::NewCf(cf)));
        }

        return true;

    } else if (cf.cftype_ == GenDb::NewCf::COLUMN_FAMILY_NOSQL) {
        cassandra::CfDef cf_def;
        std::string ret;

        cf_def.__set_keyspace(tablespace_);
        cf_def.__set_name(cf.cfname_);

        std::string key_valid_class;
        if (!DbDataTypeVecToCompositeType(key_valid_class, cf.key_validation_class))
            return false;
        cf_def.__set_key_validation_class(key_valid_class);

        std::string comparator_type;
        if (!DbDataTypeVecToCompositeType(comparator_type, cf.comparator_type))
            return false;
        cf_def.__set_comparator_type(comparator_type);

        std::string default_validation_class;
        if (!DbDataTypeVecToCompositeType(default_validation_class, cf.default_validation_class))
            return false;
        cf_def.__set_default_validation_class(default_validation_class);

        CdbIfCfInfo *cfinfo;
        if (Db_GetColumnfamily(&cfinfo, cf.cfname_)) {
            // TBD assert((*cfinfo->cfdef_.get()) == cf_def);
            cfinfo->cf_.reset(new GenDb::NewCf(cf));
        } else {
            try {
                client_->system_add_column_family(ret, cf_def);
            } catch (SchemaDisagreementException &tx) {
                CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": SchemaDisagreementException: " << tx.what());
            } catch (InvalidRequestException &tx) {
                CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": InvalidRequestException: " << tx.why);
            } catch (TApplicationException &tx) {
                CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TApplicationException: " << tx.what());
            } catch (TException &tx) {
                CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TException what: " << tx.what());
            }

            CfDef *cfdef_n = new CfDef;
            *cfdef_n = cf_def;
            std::string cfname_n = cf.cfname_;
            CdbIfCfList.insert(cfname_n, new CdbIfCfInfo(cfdef_n, new GenDb::NewCf(cf)));
        }
    } else {
        return false;
    }

    return true;
}

/*
 * called by the WorkQueue mechanism
 */
bool CdbIf::Db_AsyncAddColumn(CdbIfColList *cl) {
    bool ret_value = true;
    uint64_t ts(UTCTimestampUsec());
    GenDb::ColList *new_colp;

    if ((new_colp = cl->new_cl.get())) {
        std::map<std::string, std::map<std::string, std::vector<cassandra::Mutation> > > mutation_map;
        std::vector<cassandra::Mutation> mutations;
        GenDb::NewCf::ColumnFamilyType cftype = GenDb::NewCf::COLUMN_FAMILY_INVALID;

        for (std::vector<GenDb::NewCol>::iterator it = new_colp->columns_.begin();
                    it != new_colp->columns_.end(); it++) {
                cassandra::Mutation mutation;
                cassandra::ColumnOrSuperColumn c_or_sc;
                cassandra::Column c;

                if (it->cftype_ == GenDb::NewCf::COLUMN_FAMILY_SQL) {
                    CDBIF_CONDCHECK_LOG_RETF((it->name.size() == 1) && (it->value.size() == 1));
                    CDBIF_CONDCHECK_LOG_RETF(cftype != GenDb::NewCf::COLUMN_FAMILY_NOSQL);
                    cftype = GenDb::NewCf::COLUMN_FAMILY_SQL;

                    std::string col_name;
                    try {
                        col_name = boost::get<std::string>(it->name.at(0));
                    } catch (boost::bad_get& ex) {
                        CDBIF_HANDLE_EXCEPTION(__func__ << "Exception for boost::get, what=" << ex.what());
                    }
                    c.__set_name(col_name);
                    std::string col_value;
                    DbDataValueToStringFromCf(col_value, new_colp->cfname_, col_name, it->value.at(0));
                    c.__set_value(col_value);
                    c.__set_timestamp(ts);
                    if (cassandra_ttl_)
                        c.__set_ttl(cassandra_ttl_);

                    c_or_sc.__set_column(c);
                    mutation.__set_column_or_supercolumn(c_or_sc);
                    mutations.push_back(mutation);
                } else if (it->cftype_ == GenDb::NewCf::COLUMN_FAMILY_NOSQL) {
                    CDBIF_CONDCHECK_LOG_RETF(cftype != GenDb::NewCf::COLUMN_FAMILY_SQL);
                    cftype = GenDb::NewCf::COLUMN_FAMILY_NOSQL;

                    std::string col_name;
                    ConstructDbDataValueColumnName(col_name, new_colp->cfname_, it->name);
                    c.__set_name(col_name);

                    std::string col_value;
                    ConstructDbDataValueColumnValue(col_value, new_colp->cfname_, it->value);
                    c.__set_value(col_value);

                    c.__set_timestamp(ts);
                    if (cassandra_ttl_)
                        c.__set_ttl(cassandra_ttl_);

                    c_or_sc.__set_column(c);
                    mutation.__set_column_or_supercolumn(c_or_sc);
                    mutations.push_back(mutation);
                } else {
                    CDBIF_CONDCHECK_LOG_RETF(0);
                }
        }
        std::map<std::string, std::vector<cassandra::Mutation> > cf_map;
        cf_map.insert(std::make_pair(new_colp->cfname_, mutations));
        std::string key_value;
        ConstructDbDataValueKey(key_value, new_colp->cfname_, new_colp->rowkey_);
        mutation_map.insert(std::make_pair(key_value, cf_map));
        try {
            client_->batch_mutate(mutation_map, org::apache::cassandra::ConsistencyLevel::ONE);
        } catch (InvalidRequestException& ire) {
            CDBIF_HANDLE_EXCEPTION(__func__ << ": InvalidRequestException: " << ire.why << "for cf: " << new_colp->cfname_);
        } catch (UnavailableException& ue) {
            CDBIF_HANDLE_EXCEPTION(__func__ << "UnavailableException: " << ue.what() << "for cf: " << new_colp->cfname_);
        } catch (TimedOutException& te) {
            CDBIF_HANDLE_EXCEPTION(__func__ << "TimedOutException: " << te.what() << "for cf: " << new_colp->cfname_);
        } catch (TTransportException& te) {
            CDBIF_HANDLE_EXCEPTION(__func__ << ": TTransportException what: " << te.what());
            errhandler_();
            ret_value = false;
        } catch (TException& tx) {
            CDBIF_HANDLE_EXCEPTION(__func__ << ": TTransportException what: " << tx.what() << new_colp->cfname_);
        }
    } else {
        CDBIF_HANDLE_EXCEPTION(__func__ << ": No column info passed");
    }

    /* allocated when enqueued, free it after processing */
    delete cl;
    return ret_value;
}

bool CdbIf::NewDb_AddColumn(std::auto_ptr<GenDb::ColList> cl) {
    if (!cdbq_.get()) return false;

    CdbIfColList *qentry(new CdbIfColList(cl));
    cdbq_->Enqueue(qentry);
    return true;
}

bool CdbIf::ColListFromColumnOrSuper(GenDb::ColList& ret,
        std::vector<cassandra::ColumnOrSuperColumn>& result,
        const std::string& cfname) {
    CdbIfCfInfo *info;
    GenDb::NewCf *cf;
    if (!Db_GetColumnfamily(&info, cfname) ||
            !((cf = info->cf_.get()))) {
        return false;
    }

    if (cf->cftype_ == NewCf::COLUMN_FAMILY_SQL) {
        std::vector<NewCol>& columns = ret.columns_;
        std::vector<cassandra::ColumnOrSuperColumn>::iterator citer;
        for (citer = result.begin(); citer != result.end(); citer++) {
            GenDb::DbDataValue res;
            if (!DbDataValueFromString(res, cfname, citer->column.name, citer->column.value)) {
                CDBIF_CONDCHECK_LOG(0);
                continue;
            }
            GenDb::NewCol col(citer->column.name, res);
            columns.push_back(col);
        }
    } else if (cf->cftype_ == NewCf::COLUMN_FAMILY_NOSQL) {
        std::vector<NewCol>& columns = ret.columns_;
        std::vector<cassandra::ColumnOrSuperColumn>::iterator citer;
        for (citer = result.begin(); citer != result.end(); citer++) {
            GenDb::DbDataValueVec name;
            if (!CdbIf::DbDataValueVecFromString(name, cf->comparator_type, citer->column.name)) {
                CDBIF_CONDCHECK_LOG(0);
                continue;
            }
            GenDb::DbDataValueVec value;
            if (!CdbIf::DbDataValueVecFromString(value, cf->default_validation_class, citer->column.value)) {
                CDBIF_CONDCHECK_LOG(0);
                continue;
            }

            GenDb::NewCol col(name, value);
            columns.push_back(col);
        }
    }

    return true;
}

bool CdbIf::Db_GetRow(GenDb::ColList& ret, const std::string& cfname,
        const DbDataValueVec& rowkey) {
    std::string key;
    if (!ConstructDbDataValueKey(key, cfname, rowkey)) {
        return false;
    }

    std::vector<cassandra::ColumnOrSuperColumn> result;
    cassandra::SliceRange slicer;
    cassandra::SlicePredicate slicep;

    /* slicer has start_column and end_column as null string, which
     * means return all columns
     */
    slicep.__set_slice_range(slicer);

    cassandra::ColumnParent cparent;
    cparent.column_family.assign(cfname);

    try {
        client_->get_slice(result, key, cparent, slicep, ConsistencyLevel::ONE);
    } catch (InvalidRequestException& ire) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": InvalidRequestException: " << ire.why << "for cf: " << cfname);
    } catch (UnavailableException& ue) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": UnavailableException: " << ue.what() << "for cf: " << cfname);
    } catch (TimedOutException& te) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TimedOutException: " << te.what() << "for cf: " << cfname);
    } catch (TApplicationException& tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TApplicationException: " << tx.what() << "for cf: " << cfname);
    } catch (TException& tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TException what: " << tx.what() << "for cf: " << cfname);
    }

    return (CdbIf::ColListFromColumnOrSuper(ret, result, cfname));
}

bool CdbIf::Db_GetMultiRow(std::vector<GenDb::ColList>& ret,
        const std::string& cfname, const std::vector<DbDataValueVec>& rowkeys) {

    std::vector<DbDataValueVec>::const_iterator it = rowkeys.begin();

    while (it != rowkeys.end())  {
        std::vector<std::string> keys;

        // do query for keys in batches
        for (int i = 0; (it != rowkeys.end()) && (i <= max_query_rows); 
                it++, i++) {
            std::string key;
            if (!ConstructDbDataValueKey(key, cfname, *it)) {
                CDBIF_CONDCHECK_LOG_RETF(0);
            }
            keys.push_back(key);
        }

        std::map<std::string, std::vector<ColumnOrSuperColumn> > ret_c;
        cassandra::SliceRange slicer;
        cassandra::SlicePredicate slicep;

        /* slicer has start_column and end_column as null string, which
         * means return all columns
         */
        slicep.__set_slice_range(slicer);

        cassandra::ColumnParent cparent;
        cparent.column_family.assign(cfname);

        try {
            client_->multiget_slice(ret_c, keys, cparent, slicep, ConsistencyLevel::ONE);
        } catch (InvalidRequestException& ire) {
            CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": InvalidRequestException: " << ire.why << "for cf: " << cfname);
        } catch (UnavailableException& ue) {
            CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": UnavailableException: " << ue.what() << "for cf: " << cfname);
        } catch (TimedOutException& te) {
            CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TimedOutException: " << te.what() << "for cf: " << cfname);
        } catch (TApplicationException& tx) {
            CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TApplicationException: " << tx.what() << "for cf: " << cfname);
        } catch (TException& tx) {
            CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TException what: " << tx.what() << "for cf: " << cfname);
        }

        CdbIfCfInfo *info;
        GenDb::NewCf *cf;
        if (!Db_GetColumnfamily(&info, cfname) ||
                !((cf = info->cf_.get()))) {
            CDBIF_CONDCHECK_LOG_RETF(0);
        }

        for (std::map<std::string, std::vector<ColumnOrSuperColumn> >::iterator it = ret_c.begin();
                it != ret_c.end(); it++) {
            GenDb::ColList col_list;
            if (!CdbIf::DbDataValueVecFromString(col_list.rowkey_, cf->key_validation_class, it->first)) {
                CDBIF_CONDCHECK_LOG(0);
                continue;
            }
            CdbIf::ColListFromColumnOrSuper(col_list, it->second, cfname);
            ret.push_back(col_list);
        }

    } // while loop

    return true;
}

bool CdbIf::Db_GetRangeSlices(GenDb::ColList& col_list,
                const std::string& cfname, const GenDb::ColumnNameRange& crange,
                const GenDb::DbDataValueVec& rowkey) {
    bool result = 
        Db_GetRangeSlices_Internal(col_list, cfname, crange, rowkey);

    bool col_limit_reached = (col_list.columns_.size() == crange.count);

    GenDb::ColumnNameRange crange_new = crange;
    if (col_limit_reached && (col_list.columns_.size()>0))
    {
        // copy last entry of result returned as column start for next qry
        crange_new.start_ = (col_list.columns_.back()).name;
    }

    // extract rest of the result
    while (col_limit_reached && result)
    {
        GenDb::ColList next_col_list;

        result = 
    Db_GetRangeSlices_Internal(next_col_list, cfname, crange_new, rowkey);
        col_limit_reached = 
            (next_col_list.columns_.size() == crange.count);

        // copy last entry of result returned as column start for next qry
        if (col_limit_reached && (next_col_list.columns_.size()>0))
        {
            crange_new.start_ = (next_col_list.columns_.back()).name;
        }

        // copy result after the first entry
        std::vector<NewCol>::iterator it = next_col_list.columns_.begin();
        if (it != next_col_list.columns_.end()) it++;
        col_list.columns_.insert(
                col_list.columns_.end(), it, next_col_list.columns_.end());
    }

    return result;
}

bool CdbIf::Db_GetRangeSlices_Internal(GenDb::ColList& col_list,
                const std::string& cfname, const GenDb::ColumnNameRange& crange,
                const GenDb::DbDataValueVec& rowkey) {
    std::vector<cassandra::KeySlice> result;
    cassandra::SlicePredicate slicep;
    cassandra::SliceRange slicer;
    cassandra::KeyRange krange;

    cassandra::ColumnParent cparent;
    cparent.column_family.assign(cfname);
    cparent.super_column.assign("");

    std::string key_string;
    if (!ConstructDbDataValueKey(key_string, cfname, rowkey)) {
        CDBIF_CONDCHECK_LOG_RETF(0);
    }
    krange.__set_start_key(key_string);
    krange.__set_end_key(key_string);
    krange.__set_count(1);

    std::string start_string;
    std::string finish_string;
    if (!ConstructDbDataValueColumnName(start_string, cfname, crange.start_)) {
        CDBIF_CONDCHECK_LOG_RETF(0);
    }
    if (!ConstructDbDataValueColumnName(finish_string, cfname, crange.finish_)) {
        CDBIF_CONDCHECK_LOG_RETF(0);
    }

    slicer.__set_start(start_string);
    slicer.__set_finish(finish_string);
    slicer.__set_count(crange.count);
    slicep.__set_slice_range(slicer);

    try {
        client_->get_range_slices(result, cparent, slicep, krange, ConsistencyLevel::ONE);
    } catch (InvalidRequestException& ire) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": InvalidRequestException: " << ire.why << "for cf: " << cfname);
    } catch (UnavailableException& ue) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": UnavailableException: " << ue.what() << "for cf: " << cfname);
    } catch (TimedOutException& te) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TimedOutException: " << te.what() << "for cf: " << cfname);
    } catch (TApplicationException& tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TApplicationException: " << tx.what() << "for cf: " << cfname);
    } catch (TException& tx) {
        CDBIF_HANDLE_EXCEPTION_RETF(__func__ << ": TException what: " << tx.what() << "for cf: " << cfname);
    }

    CDBIF_CONDCHECK_LOG_RETF(result.size() <= 1);

    if (result.size() == 1) {
        cassandra::KeySlice& ks = result[0];
        CdbIf::ColListFromColumnOrSuper(col_list, ks.columns, cfname);
    }

    return true;
}

/* encode/decode for non-composite */
std::string CdbIf::Db_encode_string_non_composite(const DbDataValue& value) {
    std::string output;

    try {
        output = boost::get<std::string>(value);
    } catch (boost::bad_get& ex) {
        CDBIF_HANDLE_EXCEPTION(__func__ << "Exception for boost::get, what=" << ex.what());
    }
    return output;
}

GenDb::DbDataValue CdbIf::Db_decode_string_non_composite(const std::string& input) {
    return input;
}

std::string CdbIf::Db_encode_UUID_non_composite(const DbDataValue& value) {
    boost::uuids::uuid u;
    try {
        u = boost::get<boost::uuids::uuid>(value);
    } catch (boost::bad_get& ex) {
        CDBIF_HANDLE_EXCEPTION(__func__ << "Exception for boost::get, what=" << ex.what());
    }

    std::string u_s(u.size(), 0);
    std::copy(u.begin(), u.end(), u_s.begin());

    return u_s;
}

DbDataValue CdbIf::Db_decode_UUID_non_composite(const std::string& input) {
    boost::uuids::uuid u;
    memcpy(&u, input.c_str(), 0x0010);

    return u;
}

/*
 * Size is increased by 1 byte
 */
std::string CdbIf::Db_encode_Unsigned8_non_composite(const DbDataValue& value) {
    uint8_t temp = 0xff;
    
    try {
        temp = boost::get<uint8_t>(value);
    } catch (boost::bad_get& ex) {
        CDBIF_HANDLE_EXCEPTION(__func__ << "Exception for boost::get, what=" << ex.what());
    }

    uint8_t data[16];
    int size = 1;
    uint8_t temp1 = temp >> 8;
    while (temp1) {
        temp1 >>= 8;
        size++;
    }
    size++;

    put_value(data, size, temp);
    std::string output((const char *)data, size);

    return output;
}

DbDataValue CdbIf::Db_decode_Unsigned8_non_composite(const std::string& input) {
    uint64_t output = get_value_unaligned((const uint8_t *)(input.c_str()), input.size());
    return ((uint8_t)output);
}

std::string CdbIf::Db_encode_Unsigned16_non_composite(const DbDataValue& value) {
    uint16_t temp = 0xffff;
    
    try {
        temp = boost::get<uint16_t>(value);
    } catch (boost::bad_get& ex) {
        CDBIF_HANDLE_EXCEPTION(__func__ << "Exception for boost::get, what=" << ex.what());
    }

    uint8_t data[16];
    int size = 1;
    uint16_t temp1 = temp >> 8;
    while (temp1) {
        temp1 >>= 8;
        size++;
    }
    size++;

    put_value(data, size, temp);
    std::string output((const char *)data, size);

    return output;
}

DbDataValue CdbIf::Db_decode_Unsigned16_non_composite(const std::string& input) {
    uint64_t output = get_value_unaligned((const uint8_t *)(input.c_str()), input.size());
    return ((uint16_t)output);
}

std::string CdbIf::Db_encode_Unsigned32_non_composite(const DbDataValue& value) {
    uint32_t temp = 0xffffffff;
    
    try {
        temp = boost::get<uint32_t>(value);
    } catch (boost::bad_get& ex) {
        CDBIF_HANDLE_EXCEPTION(__func__ << "Exception for boost::get, what=" << ex.what());
    }

    uint8_t data[16];
    int size = 1;
    uint32_t temp1 = temp >> 8;
    while (temp1) {
        temp1 >>= 8;
        size++;
    }
    size++;

    put_value(data, size, temp);
    std::string output((const char *)data, size);

    return output;
}

DbDataValue CdbIf::Db_decode_Unsigned32_non_composite(const std::string& input) {
    uint64_t output = get_value_unaligned((const uint8_t *)(input.c_str()), input.size());
    return ((uint32_t)output);
}

std::string CdbIf::Db_encode_Unsigned64_non_composite(const DbDataValue& value) {
    uint64_t temp = 0xffffffffffffffff;
    
    try {
        temp = boost::get<uint64_t>(value);
    } catch (boost::bad_get& ex) {
        CDBIF_HANDLE_EXCEPTION(__func__ << "Exception for boost::get, what=" << ex.what());
    }

    uint8_t data[16];
    int size = 1;
    uint64_t temp1 = temp >> 8;
    while (temp1) {
        temp1 >>= 8;
        size++;
    }
    size++;

    put_value(data, size, temp);
    std::string output((const char *)data, size);

    return output;
}

DbDataValue CdbIf::Db_decode_Unsigned64_non_composite(const std::string& input) {
    uint64_t output = get_value_unaligned((const uint8_t *)(input.c_str()), input.size());
    return (output);
}

/* encode/decode for composite */
std::string CdbIf::Db_encode_string_composite(const DbDataValue& value) {
    std::string input;
    try {
        input = boost::get<std::string>(value);
    } catch (boost::bad_get& ex) {
        CDBIF_HANDLE_EXCEPTION(__func__ << "Exception for boost::get, what=" << ex.what());
    }

    int input_size = input.size();
    uint8_t *data = (uint8_t *)malloc(input_size+3);

    if (data == NULL) {
        CDBIF_HANDLE_EXCEPTION(__func__ << ": Malloc failed");
        return "";
    }

    int i = 0;
    put_value(data+i, 2, input_size);
    i += 2;
    memcpy(&data[i], input.c_str(), input_size);
    i += input_size;
    data[i++] = '\0';

    std::string output((const char *)data, i);

    free(data);

    return output;
}

DbDataValue CdbIf::Db_decode_string_composite(const char *input, int& used) {
    used = 0;
    uint16_t len = get_value((const uint8_t *)input, 2);
    used += 2;

    std::string output(&input[used], len);
    used += len;
    used++; // skip eoc

    return output;
}

std::string CdbIf::Db_encode_UUID_composite(const DbDataValue& value) {
    boost::uuids::uuid u;
    try {
        u = boost::get<boost::uuids::uuid>(value);
    } catch (boost::bad_get& ex) {
        CDBIF_HANDLE_EXCEPTION(__func__ << "Exception for boost::get, what=" << ex.what());
    }
    uint8_t data[32];

    int i = 0;
    put_value(data+i, 2, 0x0010);
    i += 2;
    std::string u_s(u.size(), 0);
    std::copy(u.begin(), u.end(), u_s.begin());
    memcpy(data+i, u_s.c_str(), u_s.size());
    i += 16;
    data[i++] = '\0';

    std::string output((const char *)data, i);

    return output;
}

DbDataValue CdbIf::Db_decode_UUID_composite(const char *input, int& used) {
    boost::uuids::uuid u;
    used = 0;
    uint16_t len = get_value((const uint8_t *)input, 2);
    assert(len == 0x0010);
    used += 2;

    memcpy(&u, input+used, 0x0010);
    used += 0x0010;

    used++;

    return u;
}

std::string CdbIf::Db_encode_Unsigned_int_composite(uint64_t input) {
    uint8_t data[16];

    int size = 1;
    uint64_t temp_input = input >> 8;
    while (temp_input) {
        temp_input >>= 8;
        size++;
    }
    size++; //additional byte to take care of unsigned-ness

    int i = 0;
    put_value(data+i, 2, size);
    i += 2;
    put_value(data+i, size, input);
    i += size;
    data[i++] = '\0';

    std::string output((const char *)data, i);

    return output;
}

std::string CdbIf::Db_encode_Unsigned8_composite(const DbDataValue& value) {
    uint64_t input = 0xff;
    try {
        input = boost::get<uint8_t>(value);
    } catch (boost::bad_get& ex) {
        CDBIF_HANDLE_EXCEPTION(__func__ << "Exception for boost::get, what=" << ex.what());
    }

    return(Db_encode_Unsigned_int_composite(input));
}

DbDataValue CdbIf::Db_decode_Unsigned8_composite(const char *input, int& used) {
    used = 0;
    uint16_t len = get_value((const uint8_t *)input, 2);
    used += 2;

    uint8_t output = 0;
    for (int i = 0; i < len; i++) {
        output = (output << 8) | (uint8_t)input[used++];
    }

    used++; // skip eoc

    return output;
}

std::string CdbIf::Db_encode_Unsigned16_composite(const DbDataValue& value) {
    uint64_t input = 0xffff;
    try {
        input = boost::get<uint16_t>(value);
    } catch (boost::bad_get& ex) {
        CDBIF_HANDLE_EXCEPTION(__func__ << "Exception for boost::get, what=" << ex.what());
    }

    return(Db_encode_Unsigned_int_composite(input));
}

DbDataValue CdbIf::Db_decode_Unsigned16_composite(const char *input, int& used) {
    used = 0;
    uint16_t len = get_value((const uint8_t *)input, 2);
    used += 2;

    uint16_t output = 0;
    for (int i = 0; i < len; i++) {
        output = (output << 8) | (uint8_t)input[used++];
    }

    used++; // skip eoc

    return output;
}

std::string CdbIf::Db_encode_Unsigned32_composite(const DbDataValue& value) {
    uint64_t input = 0xffffffff;
    try {
        input = boost::get<uint32_t>(value);
    } catch (boost::bad_get& ex) {
        CDBIF_HANDLE_EXCEPTION(__func__ << "Exception for boost::get, what=" << ex.what());
    }

    return(Db_encode_Unsigned_int_composite(input));
}

DbDataValue CdbIf::Db_decode_Unsigned32_composite(const char *input, int& used) {
    used = 0;
    uint16_t len = get_value((const uint8_t *)input, 2);
    used += 2;

    uint32_t output = 0;
    for (int i = 0; i < len; i++) {
        output = (output << 8) | (uint8_t)input[used++];
    }

    used++; // skip eoc

    return output;
}

std::string CdbIf::Db_encode_Unsigned64_composite(const DbDataValue& value) {
    uint64_t input = 0xffffffffffffffff;
    try {
        input = boost::get<uint64_t>(value);
    } catch (boost::bad_get& ex) {
        CDBIF_HANDLE_EXCEPTION(__func__ << "Exception for boost::get, what=" << ex.what());
    }

    return(Db_encode_Unsigned_int_composite(input));
}

DbDataValue CdbIf::Db_decode_Unsigned64_composite(const char *input, int& used) {
    used = 0;
    uint16_t len = get_value((const uint8_t *)input, 2);
    used += 2;

    uint64_t output = 0;
    for (int i = 0; i < len; i++) {
        output = (output << 8) | (uint8_t)input[used++];
    }

    used++; // skip eoc

    return output;
}

/* restart the timer on error */
void CdbIf::PeriodicTimerErrorHandler(std::string name, std::string error) {
    LOG(ERROR, __FILE__ << ":" << __LINE__ << ": " << name + " error: " + error);
}

bool CdbIf::PeriodicTimerExpired() {
    DbTxQ_s qinfo;

    qinfo.set_name(name_);
    qinfo.set_count(cdbq_->QueueCount());
    qinfo.set_enqueues(cdbq_->EnqueueCount());
    DbTxQ::Send(qinfo);

    return true;
}

