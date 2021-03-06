/*
 * Copyright (C) 2009 Emweb bvba, Kessel-Lo, Belgium.
 *
 * See the LICENSE file for terms of use.
 */

// bugfix for https://svn.boost.org/trac/boost/ticket/5722
#include <boost/asio.hpp>

#include "Wt/Http/Client"
#include "Wt/WApplication"
#include "Wt/WIOService"
#include "Wt/WEnvironment"
#include "Wt/WLogger"
#include "Wt/WServer"

#include <sstream>
#include <boost/lexical_cast.hpp>
#include <boost/system/error_code.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/enable_shared_from_this.hpp>

#ifdef WT_WITH_SSL
#include <boost/asio/ssl.hpp>

// This seems to not work very well (or at all)

#if BOOST_VERSION >= 104700
#define VERIFY_CERTIFICATE
#endif

#endif // WT_WITH_SSL

using boost::asio::ip::tcp;

namespace Wt {

LOGGER("Http.Client");

  namespace Http {

class Client::Impl : public boost::enable_shared_from_this<Client::Impl>
{
public:
  Impl(WIOService& ioService, WServer *server, const std::string& sessionId)
    : ioService_(ioService),
      resolver_(ioService_),
      timer_(ioService_),
      server_(server),
      sessionId_(sessionId),
      timeout_(0),
      maximumResponseSize_(0),
      responseSize_(0)
  { }

  virtual ~Impl() { }

  void setTimeout(int timeout) { 
    timeout_ = timeout; 
  }

  void setMaximumResponseSize(std::size_t bytes) {
    maximumResponseSize_ = bytes;
  }

  void request(const std::string& method, const std::string& server, int port,
	       const std::string& path, const Message& message)
  {
    std::ostream request_stream(&requestBuf_);
    request_stream << method << " " << path << " HTTP/1.0\r\n";
    request_stream << "Host: " << server << "\r\n";

    bool haveContentLength = false;
    for (unsigned i = 0; i < message.headers().size(); ++i) {
      const Message::Header& h = message.headers()[i];
      if (h.name() == "Content-Length")
	haveContentLength = true;
      request_stream << h.name() << ": " << h.value() << "\r\n";
    }

    if ((method == "POST" || method == "PUT" || method == "DELETE") && !haveContentLength)
      request_stream << "Content-Length: " << message.body().length() 
		     << "\r\n";

    request_stream << "Connection: close\r\n\r\n";

    if (method == "POST" || method == "PUT" || method == "DELETE")
      request_stream << message.body();

    tcp::resolver::query query(server, boost::lexical_cast<std::string>(port));

    startTimer();
    resolver_.async_resolve(query,
			    boost::bind(&Impl::handleResolve, this,
					boost::asio::placeholders::error,
					boost::asio::placeholders::iterator));
  }

  void stop()
  {
    if (socket().is_open()) {
      boost::system::error_code ignored_ec;
      socket().shutdown(tcp::socket::shutdown_both, ignored_ec);
      socket().close();
    }
  }

  Signal<boost::system::error_code, Message>& done() { return done_; }

protected:
  typedef boost::function<void(const boost::system::error_code&)>
    ConnectHandler;
  typedef boost::function<void(const boost::system::error_code&,
			       const std::size_t&)> IOHandler;

  virtual tcp::socket& socket() = 0;
  virtual void asyncConnect(tcp::endpoint& endpoint,
			    const ConnectHandler& handler) = 0;
  virtual void asyncHandshake(const ConnectHandler& handler) = 0;
  virtual void asyncWriteRequest(const IOHandler& handler) = 0;
  virtual void asyncReadUntil(const std::string& s,
			      const IOHandler& handler) = 0;
  virtual void asyncRead(const IOHandler& handler) = 0;

private:
  void startTimer()
  {
    timer_.expires_from_now(boost::posix_time::seconds(timeout_));
    timer_.async_wait(boost::bind(&Impl::timeout, shared_from_this(),
				  boost::asio::placeholders::error));
  }

  void cancelTimer()
  {
    timer_.cancel();
  }

  void timeout(const boost::system::error_code& e)
  {
    if (e != boost::asio::error::operation_aborted) {
      boost::system::error_code ignored_ec;
      socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
			ignored_ec);

      err_ = boost::asio::error::timed_out;
    }
  }

  void handleResolve(const boost::system::error_code& err,
		     tcp::resolver::iterator endpoint_iterator)
  {
    cancelTimer();

    if (!err) {
      // Attempt a connection to the first endpoint in the list.
      // Each endpoint will be tried until we successfully establish
      // a connection.
      tcp::endpoint endpoint = *endpoint_iterator;

      startTimer();
      asyncConnect(endpoint,
		   boost::bind(&Impl::handleConnect,
			       shared_from_this(),
			       boost::asio::placeholders::error,
			       ++endpoint_iterator));
    } else {
      err_ = err;
      complete();
    }
  }
 
  void handleConnect(const boost::system::error_code& err,
		     tcp::resolver::iterator endpoint_iterator)
  {
    cancelTimer();

    if (!err) {
      // The connection was successful. Do the handshake (SSL only)
      startTimer();
      asyncHandshake(boost::bind(&Impl::handleHandshake,
				 shared_from_this(),
				 boost::asio::placeholders::error));
    } else if (endpoint_iterator != tcp::resolver::iterator()) {
      // The connection failed. Try the next endpoint in the list.
      socket().close();

      handleResolve(boost::system::error_code(), endpoint_iterator);
    } else {
      err_ = err;
      complete();
    }
  }

  void handleHandshake(const boost::system::error_code& err)
  {
    cancelTimer();

    if (!err) {
      // The handshake was successful. Send the request.
      startTimer();
      asyncWriteRequest
	(boost::bind(&Impl::handleWriteRequest,
		     shared_from_this(),
		     boost::asio::placeholders::error,
		     boost::asio::placeholders::bytes_transferred));
    } else {
      err_ = err;
      complete();
    }
  }

  void handleWriteRequest(const boost::system::error_code& err,
			  const std::size_t&)
  {
    cancelTimer();

    if (!err) {
      // Read the response status line.
      startTimer();
      asyncReadUntil("\r\n",
		     boost::bind(&Impl::handleReadStatusLine,
				 shared_from_this(),
				 boost::asio::placeholders::error,
				 boost::asio::placeholders::bytes_transferred));
    } else {
      err_ = err;
      complete();
    }
  }

  bool addResponseSize(std::size_t s)
  {
    responseSize_ += s;

    if (maximumResponseSize_ && responseSize_ > maximumResponseSize_) {
      err_ = boost::asio::error::message_size;
      complete();
      return false;
    }

    return true;
  }

  void handleReadStatusLine(const boost::system::error_code& err,
			    const std::size_t& s)
  {
    cancelTimer();

    if (!err) {
      if (!addResponseSize(s))
	return;

      // Check that response is OK.
      std::istream response_stream(&responseBuf_);
      std::string http_version;
      response_stream >> http_version;
      unsigned int status_code;
      response_stream >> status_code;
      std::string status_message;
      std::getline(response_stream, status_message);
      if (!response_stream || http_version.substr(0, 5) != "HTTP/")
      {
	err_ = boost::system::errc::make_error_code
	  (boost::system::errc::protocol_error);
	complete();
      }

      LOG_DEBUG(status_code << " " << status_message);

      response_.setStatus(status_code);

      // Read the response headers, which are terminated by a blank line.
      startTimer();
      asyncReadUntil("\r\n\r\n",
		     boost::bind(&Impl::handleReadHeaders,
				 shared_from_this(),
				 boost::asio::placeholders::error,
				 boost::asio::placeholders::bytes_transferred));
    } else {
      err_ = err;
      complete();
    }
  }

  void handleReadHeaders(const boost::system::error_code& err,
			 const std::size_t& s)
  {
    cancelTimer();

    if (!err) {
      if (!addResponseSize(s))
	return;

      // Process the response headers.
      std::istream response_stream(&responseBuf_);
      std::string header;
      while (std::getline(response_stream, header) && header != "\r") {
	std::size_t i = header.find(':');
	if (i != std::string::npos) {
	  std::string name = boost::trim_copy(header.substr(0, i));
	  std::string value = boost::trim_copy(header.substr(i+1));
	  response_.addHeader(name, value);
	}
      }

      // Write whatever content we already have to output.
      if (responseBuf_.size() > 0) {
	std::stringstream ss;
	ss << &responseBuf_;
	response_.addBodyText(ss.str());
      }

      // Start reading remaining data until EOF.
      startTimer();
      asyncRead(boost::bind(&Impl::handleReadContent,
			    shared_from_this(),
			    boost::asio::placeholders::error,
			    boost::asio::placeholders::bytes_transferred));
    } else {
      err_ = err;
      complete();
    }
  }

  void handleReadContent(const boost::system::error_code& err,
			 const std::size_t& s)
  {
    cancelTimer();

    if (!err) {
      if (!addResponseSize(s))
	return;

      std::stringstream ss;
      ss << &responseBuf_;

      LOG_DEBUG(ss.str());

      response_.addBodyText(ss.str());

      // Continue reading remaining data until EOF.
      startTimer();
      asyncRead(boost::bind(&Impl::handleReadContent,
			    shared_from_this(),
			    boost::asio::placeholders::error,
			    boost::asio::placeholders::bytes_transferred));
    } else if (err != boost::asio::error::eof
	       && err != boost::asio::error::shut_down
	       && err.value() != 335544539) {
      err_ = err;
      complete();
    } else {
      complete();
    }
  }

  void complete()
  {
    if (server_)
      server_->post(sessionId_,
		    boost::bind(&Impl::emitDone, shared_from_this()));
    else
      emitDone();
  }

  void emitDone()
  {
    done_.emit(err_, response_);
  }

protected:
  WIOService& ioService_;
  tcp::resolver resolver_;
  boost::asio::streambuf requestBuf_;
  boost::asio::streambuf responseBuf_;

private:
  boost::asio::deadline_timer timer_;
  WServer *server_;
  std::string sessionId_;
  int timeout_;
  std::size_t maximumResponseSize_, responseSize_;
  boost::system::error_code err_;
  Message response_;
  Signal<boost::system::error_code, Message> done_;
};

class Client::TcpImpl : public Client::Impl
{
public:
  TcpImpl(WIOService& ioService, WServer *server, const std::string& sessionId)
    : Impl(ioService, server, sessionId),
      socket_(ioService_)
  { }

protected:
  virtual tcp::socket& socket()
  {
    return socket_;
  }

  virtual void asyncConnect(tcp::endpoint& endpoint,
			    const ConnectHandler& handler)
  {
    socket_.async_connect(endpoint, handler);
  }

  virtual void asyncHandshake(const ConnectHandler& handler)
  {
    handler(boost::system::error_code());
  }

  virtual void asyncWriteRequest(const IOHandler& handler)
  {
    boost::asio::async_write(socket_, requestBuf_, handler);
  }

  virtual void asyncReadUntil(const std::string& s,
			      const IOHandler& handler)
  {
    boost::asio::async_read_until(socket_, responseBuf_, s, handler);
  }

  virtual void asyncRead(const IOHandler& handler)
  {
    boost::asio::async_read(socket_, responseBuf_,
			    boost::asio::transfer_at_least(1), handler);
  }

private:
  tcp::socket socket_;
};

#ifdef WT_WITH_SSL

class Client::SslImpl : public Client::Impl
{
public:
  SslImpl(WIOService& ioService, WServer *server,
	  boost::asio::ssl::context& context, const std::string& sessionId,
	  const std::string& hostName)
    : Impl(ioService, server, sessionId),
      socket_(ioService_, context),
      hostName_(hostName)
  { }

protected:
  virtual tcp::socket& socket()
  {
    return socket_.next_layer();
  }

  virtual void asyncConnect(tcp::endpoint& endpoint,
			    const ConnectHandler& handler)
  {
    socket_.lowest_layer().async_connect(endpoint, handler);
  }

  virtual void asyncHandshake(const ConnectHandler& handler)
  {
#ifdef VERIFY_CERTIFICATE
    socket_.set_verify_mode(boost::asio::ssl::verify_peer);
    LOG_DEBUG("verifying that peer is " << hostName_);
    socket_.set_verify_callback
      (boost::asio::ssl::rfc2818_verification(hostName_));
#endif

    socket_.async_handshake(boost::asio::ssl::stream_base::client, handler);
  }

  virtual void asyncWriteRequest(const IOHandler& handler)
  {
    boost::asio::async_write(socket_, requestBuf_, handler);
  }

  virtual void asyncReadUntil(const std::string& s,
			      const IOHandler& handler)
  {
    boost::asio::async_read_until(socket_, responseBuf_, s, handler);
  }

  virtual void asyncRead(const IOHandler& handler)
  {
    boost::asio::async_read(socket_, responseBuf_,
			    boost::asio::transfer_at_least(1), handler);
  }

private:
  typedef boost::asio::ssl::stream<tcp::socket> ssl_socket;

  ssl_socket socket_;
  std::string hostName_;
};
#endif // WT_WITH_SSL

Client::Client(WObject *parent)
  : WObject(parent),
    ioService_(0),
    timeout_(10),
    maximumResponseSize_(64*1024)
{ }

Client::Client(WIOService& ioService, WObject *parent)
  : WObject(parent),
    ioService_(&ioService),
    timeout_(10),
    maximumResponseSize_(64*1024)
{ }

Client::~Client()
{
  abort();
}

void Client::abort()
{
  if (impl_)
    impl_->stop();

  impl_.reset();
}

void Client::setTimeout(int seconds)
{
  timeout_ = seconds;
}

void Client::setMaximumResponseSize(std::size_t bytes)
{
  maximumResponseSize_ = bytes;
}

void Client::setSslVerifyFile(const std::string& file)
{
  verifyFile_ = file;
}

bool Client::get(const std::string& url)
{
  return request(Get, url, Message());
}

bool Client::get(const std::string& url, 
		 const std::vector<Message::Header> headers)
{
  Message m(headers);
  return request(Get, url, m);
}

bool Client::post(const std::string& url, const Message& message)
{
  return request(Post, url, message);
}

bool Client::put(const std::string& url, const Message& message)
{
  return request(Put, url, message);
}

bool Client::deleteRequest(const std::string& url, const Message& message)
{
  return request(Delete, url, message);
}

bool Client::request(Http::Method method, const std::string& url,
		     const Message& message)
{
  std::string sessionId;

  WIOService *ioService = ioService_;
  WServer *server = 0;

  WApplication *app = WApplication::instance();

  if (app) {
    sessionId = app->sessionId();
    server = app->environment().server();
    ioService = &server->ioService();
  } else if (!ioService) {
    server = WServer::instance();

    if (server)
      ioService = &server->ioService();
    else {
      LOG_ERROR("requires a WIOService for async I/O");
      return false;
    }

    server = 0;
  }

  URL parsedUrl;

  if (!parseUrl(url, parsedUrl))
    return false;

  if (parsedUrl.protocol == "http") {
    impl_.reset(new TcpImpl(*ioService, server, sessionId));

#ifdef WT_WITH_SSL
  } else if (parsedUrl.protocol == "https") {
    boost::asio::ssl::context context
      (*ioService, boost::asio::ssl::context::sslv23);

#ifdef VERIFY_CERTIFICATE
    context.set_default_verify_paths();
#endif

    if (!verifyFile_.empty() || !verifyPath_.empty()) {
      if (!verifyFile_.empty())
	context.load_verify_file(verifyFile_);
      if (!verifyPath_.empty())
	context.add_verify_path(verifyPath_);
    }

    impl_.reset(new SslImpl(*ioService, 
			    server, 
			    context, 
			    sessionId, 
			    parsedUrl.host));
#endif // WT_WITH_SSL

  } else {
    LOG_ERROR("unsupported protocol: " << parsedUrl.protocol);
    return false;
  }

  impl_->done().connect(this, &Client::emitDone);
  impl_->setTimeout(timeout_);
  impl_->setMaximumResponseSize(maximumResponseSize_);

  const char *methodNames_[] = { "GET", "POST", "PUT", "DELETE" };

  LOG_DEBUG(methodNames_[method] << " " << url);

  impl_->request(methodNames_[method], 
		 parsedUrl.host, 
		 parsedUrl.port, 
		 parsedUrl.path, 
		 message);

  return true;
}

void Client::emitDone(boost::system::error_code err, const Message& response)
{
  done_.emit(err, response);
}

bool Client::parseUrl(const std::string &url, URL &parsedUrl)
{
  std::size_t i = url.find("://");
  if (i == std::string::npos) {
    LOG_ERROR("ill-formed URL: " << url);
    return false;
  }

  parsedUrl.protocol = url.substr(0, i);
  std::string rest = url.substr(i + 3);
  std::size_t j = rest.find('/');

  if (j == std::string::npos)
    parsedUrl.host = rest;
  else {
    parsedUrl.host = rest.substr(0, j);
    parsedUrl.path = rest.substr(j);
  }

  std::size_t k = parsedUrl.host.find(':');
  if (k != std::string::npos) {
    try {
      parsedUrl.port = boost::lexical_cast<int>(parsedUrl.host.substr(k + 1));
    } catch (boost::bad_lexical_cast& e) {
      LOG_ERROR("invalid port: " << parsedUrl.host.substr(k + 1));
      return false;
    }
    parsedUrl.host = parsedUrl.host.substr(0, k);
  } else {
    if (parsedUrl.protocol == "http")
      parsedUrl.port = 80;
    else if (parsedUrl.protocol == "https")
      parsedUrl.port = 443;
    else
      parsedUrl.port = 80; // protocol will not be handled anyway
  }

  return true;
}

  }
}
