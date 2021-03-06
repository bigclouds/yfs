// the caching lock server implementation

#include "lock_server_cache.h"
#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "lang/verify.h"
#include "handle.h"
#include "tprintf.h"

lock_server_cache::lock_server_cache()
{
  pthread_mutex_init(&m, NULL);
}

lock_protocol::status
lock_server_cache::acquire(lock_protocol::lockid_t lid, std::string id, int &r)
{
  ScopedLock ml(&m);

  tprintf("acquire request of lock %lld from client %s.\n", lid, id.c_str());

  std::map<lock_protocol::lockid_t, lock_t>::iterator it = locks.find(lid);
  if (it == locks.end()) {
    locks[lid] = lock_t();
    it = locks.find(lid);
  }

  while (true) {
    switch (it->second.status) {
      case lock_status::free: {
        it->second.status = lock_status::lent;
        it->second.nacquire += 1;
        it->second.owner = id;
        r = !it->second.wait_q.empty();
        tprintf("lock %lld is owned by %s now.\n", lid, it->second.owner.c_str());
        return lock_protocol::OK;
      }

      case lock_status::lent:
      case lock_status::revoked: {
        it->second.wait_q.push(id);

        if (it->second.status == lock_status::lent) {
          handle h(it->second.owner);
          rpcc *cl = h.safebind();
          int r;

          if (cl) {
            tprintf("revoking lock %lld owned by client %s.\n", lid, it->second.owner.c_str());
            it->second.status = lock_status::revoked;
            pthread_mutex_unlock(&m);
            cl->call(rlock_protocol::revoke, lid, r);
            pthread_mutex_lock(&m);
          }
        }

        return lock_protocol::RETRY;
      }
    }
  }
}

lock_protocol::status
lock_server_cache::release(lock_protocol::lockid_t lid, std::string id, int &)
{
  ScopedLock ml(&m);

  tprintf("release request of lock %lld from client %s.\n", lid, id.c_str());

  std::map<lock_protocol::lockid_t, lock_t>::iterator it = locks.find(lid);
  if (it == locks.end() || it->second.status == lock_status::free) {
    tprintf("lock %lld is not found or free.\n", lid);
    return lock_protocol::RPCERR;
  }
  if (it->second.owner != id) {
    tprintf("lock %lld is not owned by client %s.\n", lid, id.c_str());
    return lock_protocol::RPCERR;
  }

  it->second.status = lock_status::free;
  it->second.owner.clear();

  if (!it->second.wait_q.empty()) {
    std::string next = it->second.wait_q.front();
    it->second.wait_q.pop();

    handle h(next);
    rpcc *cl = h.safebind();
    int r;

    if (cl) {
      tprintf("retry lock %lld for client %s.\n", lid, next.c_str());
      pthread_mutex_unlock(&m);
      cl->call(rlock_protocol::retry, lid, r);
      pthread_mutex_lock(&m);
    }
  }

  return lock_protocol::OK;
}

lock_protocol::status
lock_server_cache::stat(lock_protocol::lockid_t lid, int &r)
{
  ScopedLock ml(&m);

  tprintf("stat request of lock %lld.\n", lid);

  std::map<lock_protocol::lockid_t, lock_t>::iterator it = locks.find(lid);
  if (it == locks.end()) {
    r = 0;
  } else {
    r = it->second.nacquire;
  }

  return lock_protocol::OK;
}
