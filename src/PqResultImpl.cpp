#include "pch.h"
#include "PqResultImpl.h"
#include "DbConnection.h"
#include "DbResult.h"
#include "DbColumnStorage.h"
#include "PqDataFrame.h"

PqResultImpl::PqResultImpl(DbResult* pRes, PGconn* pConn, const std::string& sql) :
res(pRes),
pConn_(pConn),
pSpec_(prepare(pConn, sql)),
cache(pSpec_),
complete_(false),
ready_(false),
nrows_(0),
rows_affected_(0),
group_(0),
groups_(0),
pRes_(NULL)
{

  LOG_DEBUG << sql;

  try {
    if (cache.nparams_ == 0) {
      bind();
    }
  } catch (...) {
    PQclear(pSpec_);
    pSpec_ = NULL;
    throw;
  }
}

PqResultImpl::~PqResultImpl() {
  try {
    PQclear(pSpec_);
  } catch (...) {}
}



// Cache ///////////////////////////////////////////////////////////////////////

PqResultImpl::_cache::_cache(PGresult* spec) :
names_(get_column_names(spec)),
types_(get_column_types(spec)),
ncols_(names_.size()),
nparams_(PQnparams(spec))
{
  for (int i = 0; i < nparams_; ++i)
    LOG_VERBOSE << PQparamtype(spec, i);
}


std::vector<std::string> PqResultImpl::_cache::get_column_names(PGresult* spec) {
  std::vector<std::string> names;
  int ncols_ = PQnfields(spec);
  names.reserve(ncols_);

  for (int i = 0; i < ncols_; ++i) {
    names.push_back(std::string(PQfname(spec, i)));
  }

  return names;
}

std::vector<DATA_TYPE> PqResultImpl::_cache::get_column_types(PGresult* spec)  {
  std::vector<DATA_TYPE> types;
  int ncols_ = PQnfields(spec);
  types.reserve(ncols_);

  for (int i = 0; i < ncols_; ++i) {
    Oid type = PQftype(spec, i);
    // SELECT oid, typname FROM pg_type WHERE typtype = 'b'
    switch (type) {
    case 20: // BIGINT
      types.push_back(DT_INT64);
      break;

    case 21: // SMALLINT
    case 23: // INTEGER
    case 26: // OID
      types.push_back(DT_INT);
      break;

    case 1700: // DECIMAL
    case 701: // FLOAT8
    case 700: // FLOAT
    case 790: // MONEY
      types.push_back(DT_REAL);
      break;

    case 18: // CHAR
    case 19: // NAME
    case 25: // TEXT
    case 114: // JSON
    case 1042: // CHAR
    case 1043: // VARCHAR
      types.push_back(DT_STRING);
      break;
    case 1082: // DATE
      types.push_back(DT_DATE);
      break;
    case 1083: // TIME
    case 1266: // TIMETZOID
      types.push_back(DT_TIME);
      break;
    case 1114: // TIMESTAMP
      types.push_back(DT_DATETIME);
      break;
    case 1184: // TIMESTAMPTZOID
      types.push_back(DT_DATETIMETZ);
      break;
    case 1186: // INTERVAL
    case 3802: // JSONB
    case 2950: // UUID
      types.push_back(DT_STRING);
      break;

    case 16: // BOOL
      types.push_back(DT_BOOL);
      break;

    case 17: // BYTEA
    case 2278: // NULL
      types.push_back(DT_BLOB);
      break;

    case 705: // UNKNOWN
      types.push_back(DT_STRING);
      break;

    default:
      types.push_back(DT_STRING);
      warning("Unknown field type (%d) in column %s", type, PQfname(spec, i));
    }
  }

  return types;
}

PGresult* PqResultImpl::prepare(PGconn* conn, const std::string& sql) {
  // Prepare query
  PGresult* prep = PQprepare(conn, "", sql.c_str(), 0, NULL);
  if (PQresultStatus(prep) != PGRES_COMMAND_OK) {
    PQclear(prep);
    DbConnection::conn_stop(conn, "Failed to prepare query");
  }
  PQclear(prep);

  // Retrieve query specification
  PGresult* spec = PQdescribePrepared(conn, "");
  if (PQresultStatus(spec) != PGRES_COMMAND_OK) {
    PQclear(spec);
    DbConnection::conn_stop(conn, "Failed to retrieve query result metadata");
  }

  return spec;
}

void PqResultImpl::init(bool params_have_rows) {
  ready_ = true;
  nrows_ = 0;
  complete_ = !params_have_rows;
}



// Publics /////////////////////////////////////////////////////////////////////

bool PqResultImpl::complete() {
  return complete_;
}

int PqResultImpl::n_rows_fetched() {
  return nrows_;
}

int PqResultImpl::n_rows_affected() {
  if (!ready_) return NA_INTEGER;
  if (cache.ncols_ > 0) return 0;
  return rows_affected_;
}

void PqResultImpl::bind(const List& params) {
  if (params.size() != cache.nparams_) {
    stop("Query requires %i params; %i supplied.",
         cache.nparams_, params.size());
  }

  if (params.size() == 0 && ready_) {
    stop("Query does not require parameters.");
  }

  set_params(params);

  if (params.length() > 0) {
    SEXP first_col = params[0];
    groups_ = Rf_length(first_col);
  }
  else {
    groups_ = 1;
  }
  group_ = 0;

  rows_affected_ = 0;

  bool has_params = bind_row();
  after_bind(has_params);
}

List PqResultImpl::fetch(const int n_max) {
  if (!ready_)
    stop("Query needs to be bound before fetching");

  int n = 0;
  List out;

  if (n_max != 0)
    out = fetch_rows(n_max, n);
  else
    out = peek_first_row();

  return out;
}

List PqResultImpl::get_column_info() {
  peek_first_row();

  CharacterVector names(cache.names_.begin(), cache.names_.end());

  CharacterVector types(cache.ncols_);
  for (size_t i = 0; i < cache.ncols_; i++) {
    types[i] = Rf_type2char(DbColumnStorage::sexptype_from_datatype(cache.types_[i]));
  }

  List out = Rcpp::List::create(names, types);
  out.attr("row.names") = IntegerVector::create(NA_INTEGER, -cache.ncols_);
  out.attr("class") = "data.frame";
  out.attr("names") = CharacterVector::create("name", "type");

  return out;
}



// Publics (custom) ////////////////////////////////////////////////////////////




// Privates ////////////////////////////////////////////////////////////////////

void PqResultImpl::set_params(const List& params) {
  params_ = params;
}

bool PqResultImpl::bind_row() {
  LOG_VERBOSE << "groups: " << group_ << "/" << groups_;

  if (group_ >= groups_)
    return false;

  if (ready_ || group_ > 0)
    res->finish_query();

  std::vector<const char*> c_params(cache.nparams_);
  std::vector<int> formats(cache.nparams_);
  std::vector<int> lengths(cache.nparams_);
  for (int i = 0; i < cache.nparams_; ++i) {
    if (TYPEOF(params_[i]) == VECSXP) {
      List param(params_[i]);
      if (!Rf_isNull(param[group_])) {
        Rbyte* param_value = RAW(param[group_]);
        c_params[i] = reinterpret_cast<const char*>(param_value);
        formats[i] = 1;
        lengths[i] = Rf_length(param[group_]);
      }
    }
    else {
      CharacterVector param(params_[i]);
      if (param[group_] != NA_STRING) {
        c_params[i] = CHAR(param[group_]);
      }
    }
  }

  if (!PQsendQueryPrepared(pConn_, "", cache.nparams_, &c_params[0],
                           &lengths[0], &formats[0], 0))
    conn_stop("Failed to send query");

  if (!PQsetSingleRowMode(pConn_))
    conn_stop("Failed to set single row mode");

  return true;
}

void PqResultImpl::after_bind(bool params_have_rows) {
  init(params_have_rows);
  if (params_have_rows)
    step();
}

List PqResultImpl::fetch_rows(const int n_max, int& n) {
  n = (n_max < 0) ? 100 : n_max;

  PqDataFrame data(this, cache.names_, n_max, cache.types_);

  if (complete_ && data.get_ncols() == 0) {
    warning("Don't need to call dbFetch() for statements, only for queries");
  }

  while (!complete_) {
    LOG_VERBOSE << nrows_ << "/" << n;

    data.set_col_values();
    step();
    nrows_++;
    if (!data.advance())
      break;
  }

  LOG_VERBOSE << nrows_;
  return data.get_data();
}

void PqResultImpl::step() {
  while (step_run())
    ;
}

bool PqResultImpl::step_run() {
  LOG_VERBOSE;

  pRes_ = PQgetResult(pConn_);

  // We're done, but we need to call PQgetResult until it returns NULL
  if (PQresultStatus(pRes_) == PGRES_TUPLES_OK) {
    PGresult* next = PQgetResult(pConn_);
    while (next != NULL) {
      PQclear(next);
      next = PQgetResult(pConn_);
    }
  }

  if (pRes_ == NULL) {
    PQclear(pRes_);
    stop("No active query");
  }

  ExecStatusType status = PQresultStatus(pRes_);

  switch (status) {
  case PGRES_FATAL_ERROR:
    {
      PQclear(pRes_);
      conn_stop("Failed to fetch row");
      return false;
    }
  case PGRES_SINGLE_TUPLE:
    return false;
  default:
    return step_done();
  }
}

bool PqResultImpl::step_done() {
  char* tuples = PQcmdTuples(pRes_);
  rows_affected_ += atoi(tuples);

  ++group_;
  bool more_params = bind_row();

  if (!more_params)
    complete_ = true;

  LOG_VERBOSE << "group: " << group_ << ", more_params: " << more_params;
  return more_params;
}

List PqResultImpl::peek_first_row() {
  PqDataFrame data(this, cache.names_, 1, cache.types_);

  if (!complete_)
    data.set_col_values();
  // Not calling data.advance(), remains a zero-row data frame

  return data.get_data();
}

void PqResultImpl::conn_stop(const char* msg) const {
  DbConnection::conn_stop(pConn_, msg);
}

void PqResultImpl::bind() {
  bind(List());
}

PGresult* PqResultImpl::get_result() {
  return pRes_;
}
