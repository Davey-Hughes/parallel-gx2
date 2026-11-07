/* libcurl's API over libretro-common's net_http (see curl/curl.h).
 *
 * net_http's DNS cache and connection pool belong to one thread, so
 * every transfer runs on a single transfer thread. A caller hands it a
 * transfer through a lock-free stack and takes the results - header
 * lines, body runs, the outcome - from the transfer's own
 * single-producer queue, running its callbacks on its own thread the
 * way libcurl does. Nothing takes a lock: the queues are atomics, and
 * a side with nothing to do sleeps on an eventcount.
 *
 * As in libcurl, curl_global_init() and curl_global_cleanup() are not
 * thread-safe: the core calls them once, from retro_init() and
 * retro_deinit(); curl_easy_init() calls curl_global_init() when no
 * one has. */
#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <rthreads/retro_eventcount.h>
#include <net/net_compat.h>
#include <net/net_http.h>
#include <net/net_socket_ssl.h>
#include <crypto/pk.h>
#include <crypto/x509.h>

/* net_socket_ssl_retro.c reports refused certificates through these */
extern "C" void ssl_socket_log_verify_fail(int mode_required,
      const char *domain, const char *info)
{
   (void)mode_required; (void)domain; (void)info;
}
extern "C" void ssl_socket_log_verify_disabled(const char *domain)
{
   (void)domain;
}

struct curl_tls_settings
{
   std::vector<std::vector<uint8_t>> trust;
   std::vector<uint8_t> cert;
   std::vector<uint8_t> keyDer;
   struct rsa_private_key key;
   std::vector<unsigned> suites;
   bool hasKey = false;
   bool verifyPeer = true;
   bool verifyHost = true;
   /* what net_http is given, pointing into the above */
   std::vector<const uint8_t*> trustPtr;
   std::vector<size_t> trustLen;
   const uint8_t *chainPtr = nullptr;
   size_t chainLen = 0;
   struct ssl_socket_retro_config cfg;

   const struct ssl_socket_retro_config *build()
   {
      memset(&cfg, 0, sizeof(cfg));
      trustPtr.clear();
      trustLen.clear();
      for (auto &t : trust)
      {
         trustPtr.push_back(t.data());
         trustLen.push_back(t.size());
      }
      if (!trust.empty())
      {
         cfg.trust       = trustPtr.data();
         cfg.trust_lens  = trustLen.data();
         cfg.trust_count = (unsigned)trust.size();
      }
      if (hasKey)
      {
         chainPtr        = cert.data();
         chainLen        = cert.size();
         cfg.chain       = &chainPtr;
         cfg.chain_lens  = &chainLen;
         cfg.chain_count = 1;
         cfg.key         = &key;
      }
      if (!suites.empty())
      {
         cfg.suites      = suites.data();
         cfg.suite_count = (unsigned)suites.size();
      }
      if (!verifyPeer)
         cfg.flags |= SSL_RETRO_NO_VERIFY;
      if (!verifyHost)
         cfg.flags |= SSL_RETRO_NO_HOSTNAME;
      return &cfg;
   }
};

bool curl_tls_add_trust(struct curl_tls_settings *tls, const void *der, size_t len)
{
   if (!tls || !der || !len)
      return false;
   tls->trust.emplace_back((const uint8_t*)der, (const uint8_t*)der + len);
   return true;
}

bool curl_tls_set_client(struct curl_tls_settings *tls,
      const void *cert_der, size_t cert_len, const void *key_der, size_t key_len)
{
   if (!tls || !cert_der || !cert_len || !key_der || !key_len)
      return false;
   tls->cert.assign((const uint8_t*)cert_der, (const uint8_t*)cert_der + cert_len);
   tls->keyDer.assign((const uint8_t*)key_der, (const uint8_t*)key_der + key_len);
   /* the parsed key points into keyDer */
   tls->hasKey = x509_rsa_private_key_parse(&tls->key, tls->keyDer.data(), tls->keyDer.size()) == 0;
   return tls->hasKey;
}

void curl_tls_set_suites(struct curl_tls_settings *tls, const unsigned *suites, size_t count)
{
   if (tls)
      tls->suites.assign(suites, suites + count);
}

void curl_tls_set_verify(struct curl_tls_settings *tls, bool peer, bool host)
{
   if (!tls)
      return;
   tls->verifyPeer = peer;
   tls->verifyHost = host;
}

namespace
{
   enum class EventKind { Header, Body, Done };

   struct Event
   {
      std::atomic<Event*> next{nullptr};
      EventKind kind = EventKind::Body;
      std::string data;
      CURLcode result = CURLE_OK;
      long status = 0;
   };

   /* The transfer thread stops reading a transfer whose caller has
    * this much unread. */
   constexpr size_t kMaxQueued = 1u << 20;

   void wakeTransferThread();

   /* One request: built by the caller, run by the transfer thread,
    * whose results go back through the event queue. Two owners: the
    * last of the two to let go frees it. */
   struct Transfer
   {
      std::atomic<int> refs{2};
      Transfer *nextSubmit = nullptr;
      /* request */
      std::string url;
      std::string method;
      std::string headers;
      std::string contentType;
      std::string userAgent;
      std::vector<uint8_t> body;
      bool hasBody = false;
      bool hasTls = false;
      curl_tls_settings tls;
      std::chrono::steady_clock::time_point deadline{};
      bool hasDeadline = false;
      /* caller -> transfer thread */
      std::atomic<bool> abandoned{false};
      std::atomic<bool> recvPaused{false};
      /* transfer thread -> caller: a single-producer queue whose head
       * the caller owns and whose tail the transfer thread owns */
      Event *head;
      Event *tail;
      std::atomic<uint32_t> posted{0};
      std::atomic<size_t> queuedBytes{0};
      retro_eventcount_t ec;  /* the caller sleeps here */
      /* transfer thread only */
      struct http_t *http = nullptr;
      bool headersSent = false;
      bool finished = false;

      Transfer()
      {
         head = tail = new Event(); /* stub */
         retro_eventcount_init(&ec);
      }

      ~Transfer()
      {
         retro_eventcount_free(&ec);
         while (head)
         {
            Event *n = head->next.load(std::memory_order_relaxed);
            delete head;
            head = n;
         }
      }

      void post(Event *e)
      {
         if (e->kind == EventKind::Body)
            queuedBytes.fetch_add(e->data.size(), std::memory_order_relaxed);
         tail->next.store(e, std::memory_order_release);
         tail = e;
         posted.fetch_add(1, std::memory_order_release);
         retro_eventcount_notify(&ec);
      }

      /* the caller's side: the next event, or null */
      Event *peek()
      {
         return head->next.load(std::memory_order_acquire);
      }

      void pop()
      {
         Event *n = head->next.load(std::memory_order_acquire);
         if (n->kind == EventKind::Body)
         {
            const size_t was = queuedBytes.fetch_sub(n->data.size(), std::memory_order_relaxed);
            if (was >= kMaxQueued && was - n->data.size() < kMaxQueued)
               wakeTransferThread(); /* it stopped reading for this one */
         }
         delete head;
         head = n; /* n becomes the stub */
      }

      void release()
      {
         if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete this;
      }
   };

   std::atomic<Transfer*> g_submitted{nullptr};
   retro_eventcount_t g_wake;  /* the transfer thread sleeps here */
   std::atomic<bool> g_stopping{false};
   bool g_running = false;
   std::thread g_thread;

   void wakeTransferThread()
   {
      retro_eventcount_notify(&g_wake);
   }

   CURLcode failureCode(struct http_t *h)
   {
      int code = 0;
      const char *stage = net_http_failure(h, &code);
      if (!stage)
         return CURLE_RECV_ERROR;
      if (strstr(stage, "dns"))
         return CURLE_COULDNT_RESOLVE_HOST;
      if (strstr(stage, "ssl_config"))
         return CURLE_SSL_CERTPROBLEM;
      if (strstr(stage, "ssl"))
         return CURLE_SSL_CONNECT_ERROR;
      if (strstr(stage, "connect") || strstr(stage, "socket_create"))
         return CURLE_COULDNT_CONNECT;
      if (strstr(stage, "send") || strstr(stage, "source"))
         return CURLE_SEND_ERROR;
      if (strstr(stage, "redirect"))
         return CURLE_TOO_MANY_REDIRECTS;
      return CURLE_RECV_ERROR;
   }

   /* The status line and header lines, as libcurl hands them to a
    * header callback: each with its CRLF, then an empty one. */
   void postHeaders(Transfer *t)
   {
      if (t->headersSent)
         return;
      t->headersSent = true;
      const int status = net_http_status(t->http);
      char *block = net_http_headers_take(t->http, true);
      if (status <= 0 && !block)
         return;
      Event *e = new Event();
      e->kind = EventKind::Header;
      e->status = status;
      e->data = "HTTP/1.1 " + std::to_string(status) + "\r\n";
      for (const char *l = net_http_header_next(block, nullptr); l; l = net_http_header_next(block, l))
      {
         e->data += l;
         e->data += "\r\n";
      }
      e->data += "\r\n";
      free(block);
      t->post(e);
   }

   bool sink(void *userdata, const void *data, size_t len)
   {
      Transfer *t = (Transfer*)userdata;
      if (t->abandoned.load(std::memory_order_relaxed))
         return false;
      postHeaders(t);
      Event *e = new Event();
      e->kind = EventKind::Body;
      e->data.assign((const char*)data, len);
      t->post(e);
      return true;
   }

   void finish(Transfer *t, CURLcode result)
   {
      long status = 0;
      if (t->http)
      {
         postHeaders(t);
         status = net_http_status(t->http);
         net_http_delete(t->http);
         t->http = nullptr;
      }
      Event *e = new Event();
      e->kind = EventKind::Done;
      e->result = result;
      e->status = status;
      t->post(e);
      t->finished = true;
   }

   bool start(Transfer *t)
   {
      struct http_connection_t *conn = net_http_connection_new(t->url.c_str(), t->method.c_str(), nullptr);
      if (!conn)
         return false;
      if (!t->userAgent.empty())
         net_http_connection_set_user_agent(conn, t->userAgent.c_str());
      if (!t->headers.empty())
         net_http_connection_set_headers(conn, t->headers.c_str());
      if (t->hasBody)
         net_http_connection_set_content(conn, t->contentType.empty() ? nullptr : t->contentType.c_str(),
               t->body.size(), t->body.data());
      if (t->hasTls)
         net_http_connection_set_tls(conn, t->tls.build());
      net_http_connection_set_sink(conn, sink, t);
      if (!net_http_connection_iterate(conn) || !net_http_connection_done(conn)
            || !(t->http = net_http_new(conn)))
      {
         net_http_connection_free(conn);
         return false;
      }
      net_http_connection_free(conn);
      return true;
   }

   void transferThread()
   {
      std::vector<Transfer*> active;
      net_http_init();
      for (;;)
      {
         const int key = retro_eventcount_prepare_wait(&g_wake);
         if (g_stopping.load(std::memory_order_acquire))
         {
            retro_eventcount_cancel_wait(&g_wake);
            break;
         }

         /* take the new ones, oldest first */
         Transfer *list = g_submitted.exchange(nullptr, std::memory_order_acquire);
         std::vector<Transfer*> fresh;
         for (; list; list = list->nextSubmit)
            fresh.push_back(list);
         for (auto it = fresh.rbegin(); it != fresh.rend(); ++it)
         {
            Transfer *t = *it;
            if (t->abandoned.load(std::memory_order_relaxed))
               finish(t, CURLE_ABORTED_BY_CALLBACK);
            else if (!start(t))
               finish(t, CURLE_URL_MALFORMAT);
            if (t->finished)
               t->release();
            else
               active.push_back(t);
         }

         bool moved = false;
         const auto now = std::chrono::steady_clock::now();
         for (size_t i = 0; i < active.size();)
         {
            Transfer *t = active[i];
            if (t->abandoned.load(std::memory_order_relaxed))
               finish(t, CURLE_ABORTED_BY_CALLBACK);
            else if (t->hasDeadline && now >= t->deadline)
               finish(t, CURLE_OPERATION_TIMEDOUT);
            else if (t->recvPaused.load(std::memory_order_relaxed)
                  || t->queuedBytes.load(std::memory_order_relaxed) >= kMaxQueued)
            {
               /* the caller is not reading: neither is this */
            }
            else
            {
               size_t pos = 0, total = 0;
               const uint32_t before = t->posted.load(std::memory_order_relaxed);
               if (net_http_update(t->http, &pos, &total))
               {
                  const int status = net_http_status(t->http);
                  finish(t, status > 0 ? CURLE_OK : failureCode(t->http));
               }
               if (t->posted.load(std::memory_order_relaxed) != before)
                  moved = true;
            }
            if (t->finished)
            {
               moved = true;
               t->release();
               active[i] = active.back();
               active.pop_back();
            }
            else
               i++;
         }

         if (moved)
         {
            retro_eventcount_cancel_wait(&g_wake);
            continue;
         }
         if (active.empty())
         {
            /* nothing to run: sleep until handed something */
            retro_eventcount_commit_wait(&g_wake, key);
            continue;
         }
         /* Wait on the sockets. net_http_wait() takes one transfer;
          * with more than one, the wait is short so that each gets
          * its turn, and new work waits at most that long. */
         Transfer *first = nullptr;
         for (Transfer *t : active)
            if (!t->recvPaused.load(std::memory_order_relaxed)
                  && t->queuedBytes.load(std::memory_order_relaxed) < kMaxQueued)
            {
               first = t;
               break;
            }
         if (!first)
            retro_eventcount_commit_wait(&g_wake, key);
         else
         {
            retro_eventcount_cancel_wait(&g_wake);
            net_http_wait(first->http, active.size() == 1 ? 50 : 5);
         }
      }
      for (Transfer *t : active)
      {
         finish(t, CURLE_ABORTED_BY_CALLBACK);
         t->release();
      }
      net_http_deinit();
   }

   void submit(Transfer *t)
   {
      Transfer *head = g_submitted.load(std::memory_order_relaxed);
      do
         t->nextSubmit = head;
      while (!g_submitted.compare_exchange_weak(head, t, std::memory_order_release, std::memory_order_relaxed));
      wakeTransferThread();
   }

   struct Easy
   {
      std::string url;
      std::string customRequest;
      std::string userAgent;
      std::vector<std::string> headers;
      const void *postFields = nullptr;
      const struct curl_httppost *httpPost = nullptr;
      curl_off_t postFieldSize = -1;
      curl_off_t inFileSize = -1;
      bool post = false;
      bool noBody = false;
      bool noProgress = true;
      bool verifyPeer = true;
      bool verifyHost = true;
      long timeoutMs = 0;
      curl_write_callback writeFn = nullptr;
      void *writeData = nullptr;
      curl_write_callback headerFn = nullptr;
      void *headerData = nullptr;
      curl_read_callback readFn = nullptr;
      void *readData = nullptr;
      curl_progress_callback progressFn = nullptr;
      void *progressData = nullptr;
      curl_ssl_ctx_callback sslCtxFn = nullptr;
      void *sslCtxData = nullptr;
      /* the request in flight */
      Transfer *xfer = nullptr;
      bool inMulti = false;
      bool done = false;
      CURLcode result = CURLE_OK;
      std::atomic<int> pauseBits{0};
      size_t bodyOffset = 0;  /* of the body run being delivered */
      /* what curl_easy_getinfo() reports */
      long status = 0;
      double downloaded = 0;
      double uploaded = 0;
      double contentLength = -1;
      double seconds = 0;
      std::chrono::steady_clock::time_point started;
      std::string contentType;

      void reset()
      {
         this->~Easy();
         new (this) Easy();
      }

      ~Easy()
      {
         abandon();
      }

      void abandon()
      {
         if (xfer)
         {
            xfer->abandoned.store(true, std::memory_order_relaxed);
            wakeTransferThread();
            xfer->release();
            xfer = nullptr;
         }
      }
   };

   std::string headerValue(const std::string &h)
   {
      size_t i = h.find(':') + 1;
      while (i < h.size() && (h[i] == ' ' || h[i] == '\t'))
         i++;
      return h.substr(i);
   }

   bool equalsNoCase(const char *a, const char *b, size_t n)
   {
      for (size_t i = 0; i < n; i++)
      {
         char x = a[i], y = b[i];
         if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
         if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
         if (x != y)
            return false;
      }
      return true;
   }

   bool headerIs(const std::string &h, const char *name)
   {
      const size_t n = strlen(name);
      return h.size() > n && equalsNoCase(h.c_str(), name, n) && h[n] == ':';
   }

   /* The value of header @name in a block of CRLF-ended lines. */
   bool findHeader(const std::string &block, const char *name, std::string &value)
   {
      size_t a = 0;
      while (a < block.size())
      {
         size_t b = block.find("\r\n", a);
         if (b == std::string::npos)
            b = block.size();
         const std::string line = block.substr(a, b - a);
         if (headerIs(line, name))
         {
            value = headerValue(line);
            return true;
         }
         a = b + 2;
      }
      return false;
   }

   /* Builds the transfer and hands it over. */
   CURLcode begin(CURL *handle, Easy *e)
   {
      e->abandon();
      e->done = false;
      e->result = CURLE_OK;
      e->status = 0;
      e->downloaded = 0;
      e->uploaded = 0;
      e->contentLength = -1;
      e->bodyOffset = 0;
      e->contentType.clear();
      e->pauseBits.store(0, std::memory_order_relaxed);
      e->started = std::chrono::steady_clock::now();
      if (e->url.empty())
         return CURLE_URL_MALFORMAT;

      Transfer *t = new Transfer();
      t->url = e->url;
      if (!e->customRequest.empty())
         t->method = e->customRequest;
      else if (e->noBody)
         t->method = "HEAD";
      else if (e->post)
         t->method = "POST";
      else
         t->method = "GET";
      t->userAgent = e->userAgent;
      /* net_http writes Host, Content-Length, Accept-Encoding and
       * User-Agent itself; the caller's Content-Type goes in its own
       * slot so net_http does not add a default one */
      for (const std::string &h : e->headers)
      {
         if (headerIs(h, "Host") || headerIs(h, "Content-Length") || headerIs(h, "Accept-Encoding")
               || headerIs(h, "Expect") || headerIs(h, "Transfer-Encoding"))
            continue;
         if (headerIs(h, "User-Agent"))
         {
            t->userAgent = headerValue(h);
            continue;
         }
         if (headerIs(h, "Content-Type"))
         {
            t->contentType = headerValue(h);
            continue;
         }
         if (h.find(':') == std::string::npos)
            continue; /* "Name;" sends an empty header in libcurl; dropped */
         t->headers += h;
         t->headers += "\r\n";
      }
      if (e->post)
      {
         t->hasBody = true;
         if (e->httpPost && !e->postFields)
         {
            /* multipart/form-data: each part with its name, the boundary
             * one no part can contain */
            std::string boundary = "------------------------curlcompat";
            std::string body;
            for (;;)
            {
               bool clash = false;
               for (const struct curl_httppost *f = e->httpPost; f && !clash; f = f->next)
                  clash = f->contents && std::string(f->contents, (size_t)f->contentslength).find(boundary) != std::string::npos;
               if (!clash)
                  break;
               boundary += "x";
            }
            for (const struct curl_httppost *f = e->httpPost; f; f = f->next)
            {
               body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"";
               body.append(f->name, (size_t)f->namelength);
               body += "\"\r\n";
               if (f->contenttype)
                  body += std::string("Content-Type: ") + f->contenttype + "\r\n";
               body += "\r\n";
               if (f->contents)
                  body.append(f->contents, (size_t)f->contentslength);
               body += "\r\n";
            }
            body += "--" + boundary + "--\r\n";
            t->body.assign(body.begin(), body.end());
            if (t->contentType.empty())
               t->contentType = "multipart/form-data; boundary=" + boundary;
         }
         else if (e->postFields)
         {
            const size_t n = e->postFieldSize >= 0 ? (size_t)e->postFieldSize : strlen((const char*)e->postFields);
            t->body.assign((const uint8_t*)e->postFields, (const uint8_t*)e->postFields + n);
         }
         else if (e->readFn)
         {
            /* the body, read whole */
            char buf[16384];
            for (;;)
            {
               size_t want = sizeof(buf);
               if (e->inFileSize >= 0)
               {
                  if ((curl_off_t)t->body.size() >= e->inFileSize)
                     break;
                  want = (size_t)std::min<curl_off_t>((curl_off_t)want, e->inFileSize - (curl_off_t)t->body.size());
               }
               const size_t got = e->readFn(buf, 1, want, e->readData);
               if (got == CURL_READFUNC_ABORT)
               {
                  delete t;
                  return CURLE_ABORTED_BY_CALLBACK;
               }
               if (got == 0 || got > want)
                  break;
               t->body.insert(t->body.end(), buf, buf + got);
            }
         }
         e->uploaded = (double)t->body.size();
      }
      const bool https = equalsNoCase(e->url.c_str(), "https:", e->url.size() >= 6 ? 6 : 0) && e->url.size() >= 6;
      if (https && (e->sslCtxFn || !e->verifyPeer || !e->verifyHost))
      {
         t->hasTls = true;
         if (e->sslCtxFn)
         {
            const CURLcode r = e->sslCtxFn(handle, &t->tls, e->sslCtxData);
            if (r != CURLE_OK)
            {
               delete t;
               return r;
            }
         }
         if (!e->verifyPeer)
            t->tls.verifyPeer = false;
         if (!e->verifyHost)
            t->tls.verifyHost = false;
      }
      if (e->timeoutMs > 0)
      {
         t->hasDeadline = true;
         t->deadline = e->started + std::chrono::milliseconds(e->timeoutMs);
      }
      e->xfer = t;
      submit(t);
      return CURLE_OK;
   }

   /* Ends a transfer the caller gave up on; retire() then lets go. */
   void stop(Transfer *t)
   {
      t->abandoned.store(true, std::memory_order_relaxed);
      wakeTransferThread();
   }

   /* Lets go of a transfer that is over. */
   void retire(Easy *e)
   {
      if (e->done && e->xfer)
      {
         e->xfer->release();
         e->xfer = nullptr;
      }
   }

   /* Hands what has arrived to the callbacks; e->done once the transfer
    * is over, and then retire() lets go of it. */
   void deliver(Easy *e)
   {
      Transfer *t = e->xfer;
      if (!t)
         return;
      for (Event *ev = t->peek(); ev; ev = t->peek())
      {
         if (ev->kind == EventKind::Header)
         {
            e->status = ev->status;
            if (e->headerFn)
            {
               size_t a = 0;
               while (a < ev->data.size())
               {
                  size_t b = ev->data.find("\r\n", a);
                  b = b == std::string::npos ? ev->data.size() : b + 2;
                  std::string line = ev->data.substr(a, b - a);
                  if (headerIs(line, "Content-Type"))
                  {
                     e->contentType = headerValue(line);
                     while (!e->contentType.empty() && (e->contentType.back() == '\r' || e->contentType.back() == '\n'))
                        e->contentType.pop_back();
                  }
                  if (headerIs(line, "Content-Length"))
                     e->contentLength = atof(headerValue(line).c_str());
                  if (e->headerFn(&line[0], 1, line.size(), e->headerData) != line.size())
                  {
                     e->result = CURLE_WRITE_ERROR;
                     e->done = true;
                     stop(t);
                     return;
                  }
                  a = b;
               }
            }
            else
            {
               std::string v;
               if (findHeader(ev->data, "Content-Type", v))
                  e->contentType = v;
               if (findHeader(ev->data, "Content-Length", v))
                  e->contentLength = atof(v.c_str());
            }
            t->pop();
         }
         else if (ev->kind == EventKind::Body)
         {
            if (e->pauseBits.load(std::memory_order_relaxed) & CURLPAUSE_RECV)
               return;
            size_t n = ev->data.size() - e->bodyOffset;
            if (e->writeFn)
            {
               const size_t r = e->writeFn(&ev->data[e->bodyOffset], 1, n, e->writeData);
               if (r == CURL_WRITEFUNC_PAUSE)
               {
                  /* this run again on resume */
                  e->pauseBits.fetch_or(CURLPAUSE_RECV, std::memory_order_relaxed);
                  t->recvPaused.store(true, std::memory_order_relaxed);
                  return;
               }
               if (r != n)
               {
                  e->result = CURLE_WRITE_ERROR;
                  e->done = true;
                  stop(t);
                  return;
               }
            }
            e->downloaded += (double)n;
            e->bodyOffset = 0;
            t->pop();
            if (e->progressFn && !e->noProgress
                  && e->progressFn(e->progressData, e->contentLength > 0 ? e->contentLength : 0,
                        e->downloaded, e->uploaded, e->uploaded) != 0)
            {
               e->result = CURLE_ABORTED_BY_CALLBACK;
               e->done = true;
               stop(t);
               return;
            }
         }
         else
         {
            e->result = ev->result;
            if (ev->status > 0)
               e->status = ev->status;
            e->done = true;
            e->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - e->started).count();
            t->pop();
            return; /* the caller lets go of the transfer: retire() */
         }
      }
   }

   struct Multi
   {
      std::vector<Easy*> handles;
      std::vector<bool> started;
      std::vector<bool> reported;  /* its CURLMSG_DONE is out */
      std::vector<CURLMsg> messages;
      size_t messageRead = 0;
   };
}

CURLcode curl_global_init(long flags)
{
   (void)flags;
   if (g_running)
      return CURLE_OK;
   if (!retro_eventcount_init(&g_wake))
      return CURLE_FAILED_INIT;
   network_init();
   g_stopping.store(false, std::memory_order_relaxed);
   g_thread = std::thread(transferThread);
   g_running = true;
   return CURLE_OK;
}

void curl_global_cleanup(void)
{
   if (!g_running)
      return;
   g_stopping.store(true, std::memory_order_release);
   wakeTransferThread();
   g_thread.join();
   retro_eventcount_free(&g_wake);
   g_running = false;
}

CURL *curl_easy_init(void)
{
   if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      return nullptr;
   return new Easy();
}

void curl_easy_cleanup(CURL *curl)
{
   delete (Easy*)curl;
}

void curl_easy_reset(CURL *curl)
{
   if (curl)
      ((Easy*)curl)->reset();
}

CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...)
{
   Easy *e = (Easy*)curl;
   if (!e)
      return CURLE_BAD_FUNCTION_ARGUMENT;
   va_list ap;
   va_start(ap, option);
   long lv = 0;
   void *pv = nullptr;
   curl_off_t ov = 0;
   const int opt = (int)option;
   if (opt >= CURLOPTTYPE_OFF_T && opt < CURLOPTTYPE_BLOB)
      ov = va_arg(ap, curl_off_t);
   else if (opt >= CURLOPTTYPE_OBJECTPOINT)
      pv = va_arg(ap, void*);
   else
      lv = va_arg(ap, long);
   va_end(ap);

   switch (opt)
   {
   case CURLOPT_URL: e->url = pv ? (const char*)pv : ""; break;
   case CURLOPT_CUSTOMREQUEST: e->customRequest = pv ? (const char*)pv : ""; break;
   case CURLOPT_USERAGENT: e->userAgent = pv ? (const char*)pv : ""; break;
   case CURLOPT_HTTPHEADER:
      e->headers.clear();
      for (struct curl_slist *l = (struct curl_slist*)pv; l; l = l->next)
         if (l->data)
            e->headers.emplace_back(l->data);
      break;
   case CURLOPT_POST: e->post = lv != 0; if (e->post) e->noBody = false; break;
   case CURLOPT_HTTPGET: if (lv) { e->post = false; e->noBody = false; } break;
   case CURLOPT_NOBODY: e->noBody = lv != 0; break;
   case CURLOPT_POSTFIELDS: e->postFields = pv; e->post = true; break;
   case CURLOPT_HTTPPOST: e->httpPost = (const struct curl_httppost*)pv; if (pv) e->post = true; break;
   case CURLOPT_POSTFIELDSIZE: e->postFieldSize = lv; break;
   case CURLOPT_POSTFIELDSIZE_LARGE: e->postFieldSize = ov; break;
   case CURLOPT_INFILESIZE: e->inFileSize = lv; break;
   case CURLOPT_INFILESIZE_LARGE: e->inFileSize = ov; break;
   case CURLOPT_TIMEOUT: e->timeoutMs = lv * 1000; break;
   case CURLOPT_TIMEOUT_MS: e->timeoutMs = lv; break;
   case CURLOPT_NOPROGRESS: e->noProgress = lv != 0; break;
   case CURLOPT_SSL_VERIFYPEER: e->verifyPeer = lv != 0; break;
   case CURLOPT_SSL_VERIFYHOST: e->verifyHost = lv != 0; break;
   case CURLOPT_WRITEFUNCTION: e->writeFn = (curl_write_callback)pv; break;
   case CURLOPT_WRITEDATA: e->writeData = pv; break;
   case CURLOPT_HEADERFUNCTION: e->headerFn = (curl_write_callback)pv; break;
   case CURLOPT_HEADERDATA: e->headerData = pv; break;
   case CURLOPT_READFUNCTION: e->readFn = (curl_read_callback)pv; break;
   case CURLOPT_READDATA: e->readData = pv; break;
   case CURLOPT_PROGRESSFUNCTION: e->progressFn = (curl_progress_callback)pv; break;
   case CURLOPT_PROGRESSDATA: e->progressData = pv; break;
   case CURLOPT_SSL_CTX_FUNCTION: e->sslCtxFn = (curl_ssl_ctx_callback)pv; break;
   case CURLOPT_SSL_CTX_DATA: e->sslCtxData = pv; break;
   default: break; /* accepted, without effect */
   }
   return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *curl)
{
   Easy *e = (Easy*)curl;
   if (!e)
      return CURLE_BAD_FUNCTION_ARGUMENT;
   const CURLcode r = begin(curl, e);
   if (r != CURLE_OK)
      return r;
   Transfer *t = e->xfer;
   while (!e->done)
   {
      const int key = retro_eventcount_prepare_wait(&t->ec);
      deliver(e);
      /* sleep while nothing is left that can be handed over: an empty
       * queue, or a paused one until curl_easy_pause() lifts it */
      if (!e->done && (!t->peek() || (e->pauseBits.load(std::memory_order_relaxed) & CURLPAUSE_RECV)))
         retro_eventcount_commit_wait(&t->ec, key);
      else
         retro_eventcount_cancel_wait(&t->ec);
   }
   retire(e);
   return e->result;
}

CURLcode curl_easy_pause(CURL *curl, int bitmask)
{
   Easy *e = (Easy*)curl;
   if (!e)
      return CURLE_BAD_FUNCTION_ARGUMENT;
   e->pauseBits.store(bitmask, std::memory_order_relaxed);
   if (e->xfer)
   {
      e->xfer->recvPaused.store((bitmask & CURLPAUSE_RECV) != 0, std::memory_order_relaxed);
      if (!(bitmask & CURLPAUSE_RECV))
      {
         if (e->inMulti)
         {
            deliver(e);
            retire(e);
         }
         else
            retro_eventcount_notify(&e->xfer->ec); /* the thread in curl_easy_perform() */
      }
      wakeTransferThread();
   }
   return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...)
{
   Easy *e = (Easy*)curl;
   if (!e)
      return CURLE_BAD_FUNCTION_ARGUMENT;
   va_list ap;
   va_start(ap, info);
   void *out = va_arg(ap, void*);
   va_end(ap);
   if (!out)
      return CURLE_BAD_FUNCTION_ARGUMENT;
   switch (info)
   {
   case CURLINFO_RESPONSE_CODE: *(long*)out = e->status; break;
   case CURLINFO_SSL_VERIFYRESULT: *(long*)out = 0; break;
   case CURLINFO_SIZE_DOWNLOAD: *(double*)out = e->downloaded; break;
   case CURLINFO_SIZE_UPLOAD: *(double*)out = e->uploaded; break;
   case CURLINFO_SPEED_DOWNLOAD: *(double*)out = e->seconds > 0 ? e->downloaded / e->seconds : 0; break;
   case CURLINFO_SPEED_UPLOAD: *(double*)out = e->seconds > 0 ? e->uploaded / e->seconds : 0; break;
   case CURLINFO_CONTENT_LENGTH_DOWNLOAD: *(double*)out = e->contentLength; break;
   case CURLINFO_EFFECTIVE_URL: *(const char**)out = e->url.c_str(); break;
   case CURLINFO_CONTENT_TYPE: *(const char**)out = e->contentType.empty() ? nullptr : e->contentType.c_str(); break;
   case CURLINFO_REDIRECT_URL: *(const char**)out = nullptr; break;
   default:
      switch ((int)info & CURLINFO_TYPEMASK)
      {
      case CURLINFO_STRING: *(const char**)out = nullptr; break;
      case CURLINFO_LONG: *(long*)out = 0; break;
      case CURLINFO_DOUBLE: *(double*)out = 0; break;
      case CURLINFO_OFF_T: *(curl_off_t*)out = 0; break;
      default: return CURLE_UNKNOWN_OPTION;
      }
   }
   return CURLE_OK;
}

const char *curl_easy_strerror(CURLcode code)
{
   switch (code)
   {
   case CURLE_OK: return "No error";
   case CURLE_UNSUPPORTED_PROTOCOL: return "Unsupported protocol";
   case CURLE_FAILED_INIT: return "Failed initialization";
   case CURLE_URL_MALFORMAT: return "URL using bad/illegal format or missing URL";
   case CURLE_COULDNT_RESOLVE_HOST: return "Could not resolve hostname";
   case CURLE_COULDNT_CONNECT: return "Could not connect to server";
   case CURLE_WRITE_ERROR: return "Failed writing received data to disk/application";
   case CURLE_READ_ERROR: return "Failed to open/read local data from file/application";
   case CURLE_OUT_OF_MEMORY: return "Out of memory";
   case CURLE_OPERATION_TIMEDOUT: return "Timeout was reached";
   case CURLE_SSL_CONNECT_ERROR: return "SSL connect error";
   case CURLE_ABORTED_BY_CALLBACK: return "Operation was aborted by an application callback";
   case CURLE_TOO_MANY_REDIRECTS: return "Number of redirects hit maximum amount";
   case CURLE_GOT_NOTHING: return "Server returned nothing (no headers, no data)";
   case CURLE_SEND_ERROR: return "Failed sending data to the peer";
   case CURLE_RECV_ERROR: return "Failure when receiving data from the peer";
   case CURLE_SSL_CERTPROBLEM: return "Problem with the local SSL certificate";
   case CURLE_PEER_FAILED_VERIFICATION: return "SSL peer certificate or SSH remote key was not OK";
   default: return "Unknown error";
   }
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data)
{
   struct curl_slist *n = (struct curl_slist*)malloc(sizeof(*n));
   if (!n)
      return nullptr;
   n->data = strdup(data ? data : "");
   n->next = nullptr;
   if (!list)
      return n;
   struct curl_slist *l = list;
   while (l->next)
      l = l->next;
   l->next = n;
   return list;
}

void curl_slist_free_all(struct curl_slist *list)
{
   while (list)
   {
      struct curl_slist *n = list->next;
      free(list->data);
      free(list);
      list = n;
   }
}

CURLM *curl_multi_init(void)
{
   return new Multi();
}

CURLMcode curl_multi_cleanup(CURLM *multi)
{
   Multi *m = (Multi*)multi;
   if (!m)
      return CURLM_BAD_HANDLE;
   for (Easy *e : m->handles)
      e->inMulti = false;
   delete m;
   return CURLM_OK;
}

CURLMcode curl_multi_add_handle(CURLM *multi, CURL *curl)
{
   Multi *m = (Multi*)multi;
   Easy *e = (Easy*)curl;
   if (!m)
      return CURLM_BAD_HANDLE;
   if (!e || e->inMulti)
      return CURLM_BAD_EASY_HANDLE;
   e->inMulti = true;
   m->handles.push_back(e);
   m->started.push_back(false);
   m->reported.push_back(false);
   return CURLM_OK;
}

CURLMcode curl_multi_remove_handle(CURLM *multi, CURL *curl)
{
   Multi *m = (Multi*)multi;
   if (!m)
      return CURLM_BAD_HANDLE;
   for (size_t i = 0; i < m->handles.size(); i++)
      if (m->handles[i] == (Easy*)curl)
      {
         m->handles[i]->abandon();
         m->handles[i]->inMulti = false;
         m->handles.erase(m->handles.begin() + i);
         m->started.erase(m->started.begin() + i);
         m->reported.erase(m->reported.begin() + i);
         return CURLM_OK;
      }
   return CURLM_BAD_EASY_HANDLE;
}

CURLMcode curl_multi_perform(CURLM *multi, int *running_handles)
{
   Multi *m = (Multi*)multi;
   if (!m)
      return CURLM_BAD_HANDLE;
   int running = 0;
   for (size_t i = 0; i < m->handles.size(); i++)
   {
      Easy *e = m->handles[i];
      if (!m->started[i])
      {
         m->started[i] = true;
         const CURLcode r = begin(e, e);
         if (r != CURLE_OK)
         {
            e->done = true;
            e->result = r;
         }
      }
      if (!e->done)
      {
         deliver(e);
         retire(e);
      }
      /* done here, or earlier in a curl_easy_pause() that delivered */
      if (e->done && !m->reported[i])
      {
         m->reported[i] = true;
         CURLMsg msg{};
         msg.msg = CURLMSG_DONE;
         msg.easy_handle = e;
         msg.data.result = e->result;
         m->messages.push_back(msg);
      }
      if (!e->done)
         running++;
   }
   if (running_handles)
      *running_handles = running;
   return CURLM_OK;
}

CURLMcode curl_multi_timeout(CURLM *multi, long *milliseconds)
{
   Multi *m = (Multi*)multi;
   if (!m || !milliseconds)
      return CURLM_BAD_HANDLE;
   long t = -1;
   for (size_t i = 0; i < m->handles.size(); i++)
   {
      Easy *e = m->handles[i];
      if (!m->started[i] || (e->done && !m->reported[i])
            || (e->xfer && e->xfer->peek() && !(e->pauseBits.load(std::memory_order_relaxed) & CURLPAUSE_RECV)))
      {
         t = 0;
         break;
      }
      if (!e->done)
         t = 10;
   }
   *milliseconds = t;
   return CURLM_OK;
}

CURLMcode curl_multi_setopt(CURLM *multi, CURLMoption option, ...)
{
   (void)option;
   return multi ? CURLM_OK : CURLM_BAD_HANDLE;
}

CURLMcode curl_multi_fdset(CURLM *multi, void *read_fd_set, void *write_fd_set, void *exc_fd_set, int *max_fd)
{
   (void)read_fd_set; (void)write_fd_set; (void)exc_fd_set;
   if (!multi)
      return CURLM_BAD_HANDLE;
   if (max_fd)
      *max_fd = -1;
   return CURLM_OK;
}

CURLMsg *curl_multi_info_read(CURLM *multi, int *msgs_in_queue)
{
   Multi *m = (Multi*)multi;
   if (!m || m->messageRead >= m->messages.size())
   {
      if (m)
      {
         m->messages.clear();
         m->messageRead = 0;
      }
      if (msgs_in_queue)
         *msgs_in_queue = 0;
      return nullptr;
   }
   CURLMsg *msg = &m->messages[m->messageRead++];
   if (msgs_in_queue)
      *msgs_in_queue = (int)(m->messages.size() - m->messageRead);
   return msg;
}

namespace
{
   /* a form part's own storage: the struct, then what was copied */
   struct FormPart
   {
      struct curl_httppost post;
      std::string name;
      std::string contents;
      std::string contentType;
   };
}

CURLFORMcode curl_formadd(struct curl_httppost **httppost, struct curl_httppost **last_post, ...)
{
   if (!httppost || !last_post)
      return CURL_FORMADD_NULL;
   FormPart *part = new FormPart();
   memset(&part->post, 0, sizeof(part->post));
   const char *name = nullptr, *contents = nullptr;
   long nameLen = -1, contentsLen = -1;
   bool copyName = false, copyContents = false;
   CURLFORMcode r = CURL_FORMADD_OK;
   va_list ap;
   va_start(ap, last_post);
   for (int opt = va_arg(ap, int); opt != CURLFORM_END && r == CURL_FORMADD_OK; opt = va_arg(ap, int))
   {
      switch (opt)
      {
      case CURLFORM_COPYNAME:
      case CURLFORM_PTRNAME:
         if (name)
            r = CURL_FORMADD_OPTION_TWICE;
         name = va_arg(ap, const char*);
         copyName = opt == CURLFORM_COPYNAME;
         break;
      case CURLFORM_NAMELENGTH:
         nameLen = va_arg(ap, long);
         break;
      case CURLFORM_COPYCONTENTS:
      case CURLFORM_PTRCONTENTS:
         if (contents)
            r = CURL_FORMADD_OPTION_TWICE;
         contents = va_arg(ap, const char*);
         copyContents = opt == CURLFORM_COPYCONTENTS;
         break;
      case CURLFORM_CONTENTSLENGTH:
         contentsLen = va_arg(ap, long);
         break;
      case CURLFORM_CONTENTTYPE:
         part->contentType = va_arg(ap, const char*);
         break;
      default:
         r = CURL_FORMADD_UNKNOWN_OPTION;
      }
   }
   va_end(ap);
   if (r == CURL_FORMADD_OK && !name)
      r = CURL_FORMADD_INCOMPLETE;
   if (r != CURL_FORMADD_OK)
   {
      delete part;
      return r;
   }
   /* the part keeps its own copy whichever was asked for: the caller's
    * buffer may be gone by the time the request is built */
   (void)copyName;
   (void)copyContents;
   part->name.assign(name, nameLen >= 0 ? (size_t)nameLen : strlen(name));
   if (contents)
      part->contents.assign(contents, contentsLen >= 0 ? (size_t)contentsLen : strlen(contents));
   part->post.name = &part->name[0];
   part->post.namelength = (long)part->name.size();
   part->post.contents = contents ? &part->contents[0] : nullptr;
   part->post.contentslength = (long)part->contents.size();
   part->post.contenttype = part->contentType.empty() ? nullptr : &part->contentType[0];
   if (*last_post)
      (*last_post)->next = &part->post;
   else
      *httppost = &part->post;
   *last_post = &part->post;
   return CURL_FORMADD_OK;
}

void curl_formfree(struct curl_httppost *form)
{
   while (form)
   {
      struct curl_httppost *next = form->next;
      delete (FormPart*)form; /* post is FormPart's first member */
      form = next;
   }
}

CURLSH *curl_share_init(void)
{
   /* nothing is shared between handles that the transfer thread does
    * not share already */
   return malloc(1);
}

CURLSHcode curl_share_setopt(CURLSH *share, CURLSHoption option, ...)
{
   (void)option;
   return share ? CURLSHE_OK : CURLSHE_INVALID;
}

CURLSHcode curl_share_cleanup(CURLSH *share)
{
   free(share);
   return CURLSHE_OK;
}
