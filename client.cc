// Copyright (c) 2014, Emmanuel Goossaert. All rights reserved.
// Use of this source code is governed by the BSD 3-Clause License,
// that can be found in the LICENSE file.

#include <string>
#include <sstream>
#include <vector>
#include <libmemcached/memcached.hpp>
#include "status.h"
#include "logger.h"
#include "threadpool.h"
#include "kdb.h"

namespace kdb {

class Client {
 public:
  Client(std::string database) {
    memc = memcached(database.c_str(), database.length());
  }
  ~Client() {
    memcached_free(memc);
  }

  Status Get(const std::string& key, std::string *value_out) {
    char* buffer = new char[SIZE_BUFFER_CLIENT];
    memcached_return_t rc;
    const char* keys[1];
    keys[0] = key.c_str();
    size_t key_length[]= {key.length()};
    uint32_t flags;

    char return_key[MEMCACHED_MAX_KEY];
    size_t return_key_length;
    char *return_value;
    size_t return_value_length;

    rc = memcached_mget(memc, keys, key_length, 1);
    if (rc != MEMCACHED_SUCCESS) {
      std::string msg = key + " " + memcached_strerror(memc, rc);
      return Status::IOError(msg);
    }

    while ((return_value = memcached_fetch(memc,
                                           return_key,
                                           &return_key_length,
                                           &return_value_length,
                                           &flags,
                                           &rc))) {
        memcpy(buffer, return_value, return_value_length);
        buffer[return_value_length] = '\0';
        *value_out = buffer;
        free(return_value);
    }

    delete[] buffer;

    if (rc != MEMCACHED_END) {
      std::string msg = key + " " + memcached_strerror(memc, rc);
      return Status::IOError(msg);
    }

    return Status::OK(); 
  }


  Status Set(const std::string& key, const std::string& value) {
    memcached_return_t rc = memcached_set(memc, key.c_str(), key.length(), value.c_str(), value.length(), (time_t)0, (uint32_t)0);
    if (rc != MEMCACHED_SUCCESS) {
      std::string msg = key + " " + memcached_strerror(memc, rc);
      return Status::IOError(msg);
    }
    return Status::OK();
  }

  Status Set(const char* key, uint64_t size_key, const char *value, uint64_t size_value) {
    memcached_return_t rc = memcached_set(memc, key, size_key, value, size_value, (time_t)0, (uint32_t)0);
    if (rc != MEMCACHED_SUCCESS) {
      std::string msg = std::string(key) + " " + memcached_strerror(memc, rc);
      return Status::IOError(msg);
    }
    return Status::OK();
  }

 private:
  memcached_st *memc;
};




class ClientTask: public Task {
 public:
  ClientTask(std::string database, int num_items) {
    database_ = database;
    num_items_ = num_items;
  }
  virtual ~ClientTask() {};

  virtual void RunInLock(std::thread::id tid) {
    //std::cout << "Thread " << tid << std::endl;
  }

  virtual void Run(std::thread::id tid) {
    Client client(database_);
    int size = SIZE_LARGE_TEST_ITEMS;
    char *buffer_large = new char[size+1];
    for (auto i = 0; i < size; i++) {
      buffer_large[i] = 'a';
    }
    buffer_large[size] = '\0';


    std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();

    for (auto i = 0; i < num_items_; i++) {
      std::stringstream ss;
      std::stringstream ss_value;
      ss << tid << "-" << i;
      ss_value << "val-" << tid;

      if (false && i % 10 == 0) {
        ss_value << "-" << buffer_large;
      }

      //std::cout << ss.str() << std::endl;
      Status s = client.Set(ss.str().c_str(), ss.str().size(), buffer_large, 100);
      LOG_TRACE("ClientTask", "Set(%s): [%s]", ss.str().c_str(), s.ToString().c_str());

      if (false && i > 10) {
        std::string value;
        std::stringstream ss_get;
        ss_get << tid << "-" << (i/2);
        s = client.Get(ss_get.str(), &value);
        LOG_TRACE("ClientTask", "Get(%s): value_size:[%d] => %s", ss_get.str().c_str(), value.size(), s.ToString().c_str());
        if (value.size() < 128) {
          LOG_TRACE("ClientTask", "Get(%s): value [%s]", ss_get.str().c_str(), value.c_str());
        }
      }
    }

    std::chrono::high_resolution_clock::time_point end = std::chrono::high_resolution_clock::now();
    uint64_t duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Thread " << tid << ": done in " << duration << " ms" << std::endl;
    delete[] buffer_large;
  }

  std::string database_;
  int num_items_;
};

};


void show_usage(char *program_name) {
  printf("Example: %s --host 127.0.0.1:3490 --num-threads 120 --num-items 10000\n", program_name);
}


int main(int argc, char **argv) {
  if (argc == 1) {
    show_usage(argv[0]); 
    exit(0);
  }

  if (argc % 2 == 0) {
    std::cerr << "Error: invalid number of arguments" << std::endl; 
    show_usage(argv[0]); 
    exit(-1);
  }

  std::string host("");
  int num_threads = 0;
  int num_items = 0;

  if (argc > 2) {
    for (int i = 1; i < argc; i += 2 ) {
      if (strcmp(argv[i], "--host" ) == 0) {
        host = "--SERVER=" + std::string(argv[i+1]);
      } else if (strcmp(argv[i], "--num-items" ) == 0) {
        num_items = atoi(argv[i+1]);
      } else if (strcmp(argv[i], "--num-threads" ) == 0) {
        num_threads = atoi(argv[i+1]);
      } else {
        fprintf(stderr, "Unknown parameter [%s]\n", argv[i]);
        exit(-1); 
      }
    }
  }

  if (host == "" || num_items == 0 || num_threads == 0) {
    fprintf(stderr, "Missing arguments\n");
    exit(-1); 
  }

  kdb::ThreadPool tp(num_threads);
  tp.Start();
  for (auto i = 0; i < num_threads; i++ ) {
    tp.AddTask(new kdb::ClientTask(host, num_items));
  }
  return 0;


}

int main2(int argc, char **argv) {
  memcache::Memcache client(argv[1]);
  printf("main() start\n");
  //client.set("key", some_vector_of_chars, time_to_live, flags);
  time_t expiry= 0;
  uint32_t flags= 0;
  std::vector<char> value;
  value.push_back('a');
  value.push_back('b');
  std::string key = "mykey";
  client.set(key, value, expiry, flags);
  printf("main() stop\n");
  return 0;
}