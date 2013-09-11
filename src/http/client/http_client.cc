/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "http_client.h"
#include <boost/bind.hpp>
#include "base/task_annotations.h"
#include "io/event_manager.h"
#include "http_curl.h"

using namespace std;
using tbb::mutex;

HttpClientSession::HttpClientSession(HttpClient *client, Socket *socket) 
    : TcpSession(client, socket) , delete_called_(0) {
        set_observer(boost::bind(&HttpClientSession::OnEvent, this, _1, _2));
}

void HttpClientSession::OnRead(Buffer buffer) {
    return;
}

void HttpClientSession::OnEvent(TcpSession *session, Event event) {
    if (event == CLOSE) {
        goto error;
    }
    if (event == ACCEPT) {
        goto error;
    }
    if (event == CONNECT_COMPLETE) {
        goto error;
    }
    if (event == CONNECT_FAILED) {
        goto error;
    }
    if (event == EVENT_NONE) {
        goto error;
    }
    return;

error:
    // Call callback function with error;
    return;
}

HttpConnection::HttpConnection(boost::asio::ip::tcp::endpoint ep, size_t id, 
                               HttpClient *client) :
    endpoint_(ep), id_(id), cb_(NULL), offset_(0), curl_handle_(NULL),
    session_(NULL), client_(client) {
}

HttpConnection::~HttpConnection() {
    if (session_) {
        {
            tbb::mutex::scoped_lock lock(session_->mutex());
            session_->SetConnection(NULL);
        }
        client_->DeleteSession(session_);
        set_session(NULL);
    }
}

std::string HttpConnection::make_url(std::string &path) {
    std::ostringstream ret;

    ret << "http://" << endpoint_.address().to_string();
    if (endpoint_.port() != 0) {
        ret << ":" << endpoint_.port();
    }
    if (path.size() > 0)
        ret << "/" << path;

    return ret.str();
}

HttpClientSession *HttpConnection::CreateSession() {
    HttpClientSession *session = 
        static_cast<HttpClientSession *>(client_->CreateSession());
    if (session) {
        session->SetConnection(this);
    }
    return session;
}

void HttpConnection::set_session(HttpClientSession *session) {
    session_ = session;
}

void HttpConnection::HttpGetInternal(std::string path, HttpCb cb) {

    if (client()->AddConnection(this) == false) {
        // connection already exists
        return;
    }

    struct _GlobalInfo *gi = client()->GlobalInfo();
    struct _ConnInfo *curl_handle = new_conn(this, gi);
    curl_handle->connection = this;
    set_curl_handle(curl_handle);

    cb_ = cb;

    std::string url = make_url(path);
    set_url(curl_handle_, url.c_str());

    http_get(curl_handle_, gi);
}

int HttpConnection::HttpGet(std::string &path, HttpCb cb) {
    client()->ProcessEvent(boost::bind(&HttpConnection::HttpGetInternal,
                           this, path, cb));
    return 0;
}

void HttpConnection::HttpPutInternal(std::string put_string, std::string path,  HttpCb cb) {

    if (client()->AddConnection(this) == false) {
        // connection already exists
        return;
    }

    struct _GlobalInfo *gi = client()->GlobalInfo();
    struct _ConnInfo *curl_handle = new_conn(this, gi);
    curl_handle->connection = this;
    set_curl_handle(curl_handle);

    cb_ = cb;

    std::string url = make_url(path);
    set_url(curl_handle_, url.c_str());
    set_put_string(curl_handle_, put_string.c_str());

    http_put(curl_handle_, gi);
}

int HttpConnection::HttpPut(std::string &put_string, 
                            std::string &path,  HttpCb cb) {
    client()->ProcessEvent(boost::bind(&HttpConnection::HttpPutInternal,
                           this, put_string, path, cb));
    return 0;
                            
}

void HttpConnection::AssignData(const char *ptr, size_t size) {

    buf_.assign(ptr, size);

    // callback to client
    boost::system::error_code error;
    cb_(buf_, error);
}

const std::string &HttpConnection::GetData() {
    return buf_;
}

void HttpConnection::UpdateOffset(size_t bytes) {
    offset_ += bytes;
}

size_t HttpConnection::GetOffset() {
    return offset_;
}

HttpClient::HttpClient(EventManager *evm) : 
  TcpServer(evm) , 
  curl_timer_(TimerManager::CreateTimer(*evm->io_service(), "http client",
              TaskScheduler::GetInstance()->GetTaskId("http client"), 0)),
  id_(0), work_queue_(TaskScheduler::GetInstance()->GetTaskId("http client"), 0,
              boost::bind(&HttpClient::DequeueEvent, this, _1)) { 
    gi_ = (struct _GlobalInfo *)malloc(sizeof(struct _GlobalInfo));
    memset(gi_, 0, sizeof(struct _GlobalInfo));
}

void HttpClient::ShutdownInternal() {

    for (HttpConnectionMap::iterator iter = map_.begin(), next = iter;
         iter != map_.end(); iter = next) {
        next++;
        RemoveConnectionInternal(iter->second);
    }

    curl_multi_cleanup(gi_->multi);
    TimerManager::DeleteTimer(curl_timer_);
    SessionShutdown();
    
    assert(!map_.size());
}

void HttpClient::Shutdown() {
    work_queue_.Enqueue(boost::bind(&HttpClient::ShutdownInternal, 
                        this));
}

HttpClient::~HttpClient() {
    free(gi_);
}

void HttpClient::Init() {
    curl_init(this);
}

void HttpClient::SessionShutdown() {
    TcpServer::Shutdown();
}

boost::asio::io_service *HttpClient::io_service() {
    return this->event_manager()->io_service();
};

TcpSession *HttpClient::AllocSession(Socket *socket) {
    HttpClientSession *session = new HttpClientSession(this, socket);
    return session;
}

TcpSession *HttpClient::CreateSession() {
    TcpSession *session = TcpServer::CreateSession();
    Socket *socket = session->socket();
    boost::system::error_code err;
    socket->open(boost::asio::ip::tcp::v4(), err);

    if (err) {
        return NULL;
    }

    err = session->SetSocketOptions();
    return session;
}

HttpConnection *HttpClient::CreateConnection(boost::asio::ip::tcp::endpoint ep) {
    HttpConnection *conn = new HttpConnection(ep, ++id_, this);
    return conn;
}

bool HttpClient::AddConnection(HttpConnection *conn) {
    Key key = std::make_pair(conn->endpoint(), conn->id());
    if (map_.find(key) == map_.end()) {
        map_.insert(key, conn);
        return true;
    }
    return false;
}

void HttpClient::RemoveConnection(HttpConnection *connection) {
    work_queue_.Enqueue(boost::bind(&HttpClient::RemoveConnectionInternal, 
                                     this, connection));
}

void HttpClient::ProcessEvent(EnqueuedCb cb) {
    work_queue_.Enqueue(cb);
}

void HttpClient::TimerErrorHandler(std::string name, std::string error) {
}

bool HttpClient::TimerCb() {
    timer_cb(gi_);
    return false;
}

void HttpClient::StartTimer(long timeout_ms) {
    CancelTimer();
    curl_timer_->Start(timeout_ms, boost::bind(&HttpClient::TimerCb, this)); 
}

void HttpClient::CancelTimer() {
    curl_timer_->Cancel();
}

bool HttpClient::IsErrorHard(const boost::system::error_code &ec) {
    return TcpSession::IsSocketErrorHard(ec);
}

void HttpClient::RemoveConnectionInternal(HttpConnection *connection) {
    boost::asio::ip::tcp::endpoint endpoint = connection->endpoint();
    size_t id = connection->id();
    del_conn(connection, gi_);
    map_.erase(std::make_pair(endpoint, id));
    return;
}

bool HttpClient::DequeueEvent(EnqueuedCb cb) {
    cb();
    return true;
}
