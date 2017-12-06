#include "pch.h"
#include "DbConnection.h"
#include "encode.h"


DbConnection::DbConnection(std::vector<std::string> keys, std::vector<std::string> values) :
pCurrentResult_(NULL),
transacting_(false)
{
  size_t n = keys.size();
  std::vector<const char*> c_keys(n + 1), c_values(n + 1);

  for (size_t i = 0; i < n; ++i) {
    c_keys[i] = keys[i].c_str();
    c_values[i] = values[i].c_str();
  }
  c_keys[n] = NULL;
  c_values[n] = NULL;

  pConn_ = PQconnectdbParams(&c_keys[0], &c_values[0], false);

  if (PQstatus(pConn_) != CONNECTION_OK) {
    std::string err = PQerrorMessage(pConn_);
    PQfinish(pConn_);
    stop(err);
  }

  PQsetClientEncoding(pConn_, "UTF-8");
}

DbConnection::~DbConnection() {
  disconnect();
}

void DbConnection::disconnect() {
  try {
    PQfinish(pConn_);
    pConn_ = NULL;
  } catch (...) {}
}

PGconn* DbConnection::conn() {
  return pConn_;
}

void DbConnection::set_current_result(const DbResult* pResult) {
  // Cancels previous query, if needed.
  if (pResult == pCurrentResult_)
    return;

  if (pCurrentResult_ != NULL) {
    if (pResult != NULL)
      warning("Cancelling previous query");

    cleanup_query();
  }
  pCurrentResult_ = pResult;
}

void DbConnection::cancel_query() {
  check_connection();

  // Cancel running query
  PGcancel* cancel = PQgetCancel(pConn_);
  if (cancel == NULL) {
    warning("Failed to cancel running query");
    return;
  }

  char errbuf[256];
  if (!PQcancel(cancel, errbuf, sizeof(errbuf))) {
    warning(errbuf);
  }

  PQfreeCancel(cancel);
}

void DbConnection::finish_query() const {
  // Clear pending results
  PGresult* result;
  while ((result = PQgetResult(pConn_)) != NULL) {
    PQclear(result);
  }
}

bool DbConnection::is_current_result(const DbResult* pResult) {
  return pCurrentResult_ == pResult;
}

bool DbConnection::has_query() {
  return pCurrentResult_ != NULL;
}

void DbConnection::copy_data(std::string sql, List df) {
  LOG_DEBUG << sql;

  R_xlen_t p = df.size();
  if (p == 0)
    return;

  PGresult* pInit = PQexec(pConn_, sql.c_str());
  if (PQresultStatus(pInit) != PGRES_COPY_IN) {
    PQclear(pInit);
    conn_stop("Failed to initialise COPY");
  }
  PQclear(pInit);


  std::string buffer;
  int n = Rf_length(df[0]);
  // Sending row at-a-time is faster, presumable because it avoids copies
  // of buffer. Sending data asynchronously appears to be no faster.
  for (int i = 0; i < n; ++i) {
    buffer.clear();
    encode_row_in_buffer(df, i, buffer);

    if (PQputCopyData(pConn_, buffer.data(), static_cast<int>(buffer.size())) != 1) {
      conn_stop("Failed to put data");
    }
  }


  if (PQputCopyEnd(pConn_, NULL) != 1) {
    conn_stop("Failed to finish COPY");
  }

  PGresult* pComplete = PQgetResult(pConn_);
  if (PQresultStatus(pComplete) != PGRES_COMMAND_OK) {
    PQclear(pComplete);
    conn_stop("COPY returned error");
  }
  PQclear(pComplete);
}

void DbConnection::check_connection() {
  if (!pConn_) {
    stop("Disconnected");
  }

  ConnStatusType status = PQstatus(pConn_);
  if (status == CONNECTION_OK) return;

  // Status was bad, so try resetting.
  PQreset(pConn_);
  status = PQstatus(pConn_);
  if (status == CONNECTION_OK) return;

  conn_stop("Lost connection to database");
}

List DbConnection::info() {
  check_connection();

  const char* dbnm = PQdb(pConn_);
  const char* host = PQhost(pConn_);
  const char* port = PQport(pConn_);
  const char* user = PQuser(pConn_);
  int pver = PQprotocolVersion(pConn_);
  int sver = PQserverVersion(pConn_);
  int pid = PQbackendPID(pConn_);
  return
    List::create(
      _["dbname"] = dbnm == NULL ? "" : std::string(dbnm),
      _["host"]   = host == NULL ? "" : std::string(host),
      _["port"]   = port == NULL ? "" : std::string(port),
      _["user"]   = user == NULL ? "" : std::string(user),
      _["protocol_version"]   = pver,
      _["server_version"]     = sver,
      _["pid"]                = pid
    );
}

SEXP DbConnection::quote_string(const String& x) {
  // Returns a single CHRSXP
  check_connection();

  if (x == NA_STRING)
    return get_null_string();

  char* pq_escaped = PQescapeLiteral(pConn_, x.get_cstring(), static_cast<size_t>(-1));
  SEXP escaped = Rf_mkCharCE(pq_escaped, CE_UTF8);
  PQfreemem(pq_escaped);

  return escaped;
}

SEXP DbConnection::quote_identifier(const String& x) {
  // Returns a single CHRSXP
  check_connection();

  char* pq_escaped = PQescapeIdentifier(pConn_, x.get_cstring(), static_cast<size_t>(-1));
  SEXP escaped = Rf_mkCharCE(pq_escaped, CE_UTF8);
  PQfreemem(pq_escaped);

  return escaped;
}

SEXP DbConnection::get_null_string() {
  static RObject null = Rf_mkCharCE("NULL", CE_UTF8);
  return null;
}

bool DbConnection::is_transacting() const {
  return transacting_;
}

void DbConnection::set_transacting(bool transacting) {
  transacting_ = transacting;
}

void DbConnection::conn_stop(const char* msg) {
  conn_stop(conn(), msg);
}

void DbConnection::conn_stop(PGconn* conn, const char* msg) {
  stop("%s: %s", msg, PQerrorMessage(conn));
}

void DbConnection::cleanup_query() {
  cancel_query();
  finish_query();
}
