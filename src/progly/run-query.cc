/*
* Copyright (C) 2018 The Regents of the University of California
* All Rights Reserved
*
* This library can redistribute it and/or modify under the terms
* of the GNU Lesser General Public License Version 2.1 as published
* by the Free Software Foundation.
*
*/


#include <iostream>
#include <thread>
#include <atomic>
#include <fstream>
#include <algorithm>
#include <condition_variable>
#include <boost/program_options.hpp>
#include "include/rados/librados.hpp"
#include "cls/tabular/cls_tabular.h"
#include "cls/tabular/cls_tabular_utils.h"
#include "re2/re2.h"

namespace po = boost::program_options;

static inline uint64_t __getns(clockid_t clock)
{
  struct timespec ts;
  int ret = clock_gettime(clock, &ts);
  assert(ret == 0);
  return (((uint64_t)ts.tv_sec) * 1000000000ULL) + ts.tv_nsec;
}

static inline uint64_t getns()
{
  return __getns(CLOCK_MONOTONIC);
}

#define checkret(r,v) do { \
  if (r != v) { \
    fprintf(stderr, "error %d/%s\n", r, strerror(-r)); \
    assert(0); \
    exit(1); \
  } } while (0)

static std::string string_ncopy(const char* buffer, std::size_t buffer_size) {
  const char* copyupto = std::find(buffer, buffer + buffer_size, 0);
  return std::string(buffer, copyupto);
}

static std::mutex print_lock;

static bool quiet;
static bool use_cls;
static std::string query;
static bool use_index;
static bool projection;
static uint32_t build_index_batch_size;
static uint64_t extra_row_cost;

struct timing {
  uint64_t dispatch;
  uint64_t response;
  uint64_t read_ns;
  uint64_t eval_ns;
  uint64_t eval2_ns;
};

static std::vector<timing> timings;

// query parameters to be encoded into query_op struct
static double extended_price;
static int order_key;
static int line_number;
static int ship_date_low;
static int ship_date_high;
static double discount_low;
static double discount_high;
static double quantity;
static std::string comment_regex;
static std::string table_schema_str;
static std::string query_schema_str;
static bool fastpath;

static std::atomic<unsigned> result_count;
static std::atomic<unsigned> rows_returned;

// total number of rows processed, client side or server side (cls).
static std::atomic<unsigned> nrows_processed;

struct AioState {
  ceph::bufferlist bl;
  librados::AioCompletion *c;
  timing times;
};

// rename work_lock
static int outstanding_ios;
static std::vector<std::string> target_objects;
static std::list<AioState*> ready_ios;

static std::mutex dispatch_lock;
static std::condition_variable dispatch_cond;

static std::mutex work_lock;
static std::condition_variable work_cond;

static bool stop;

static void print_row(const char *row)
{
  if (quiet)
    return;

  print_lock.lock();

  const size_t order_key_field_offset = 0;
  size_t line_number_field_offset;
  if (projection && use_cls)
    line_number_field_offset = 4;
  else
    line_number_field_offset = 12;
  const size_t quantity_field_offset = 16;
  const size_t extended_price_field_offset = 24;
  const size_t discount_field_offset = 32;
  const size_t shipdate_field_offset = 50;
  const size_t comment_field_offset = 97;
  const size_t comment_field_length = 44;

  const double extended_price = *((const double *)(row + extended_price_field_offset));
  const int order_key = *((const int *)(row + order_key_field_offset));
  const int line_number = *((const int *)(row + line_number_field_offset));
  const int ship_date = *((const int *)(row + shipdate_field_offset));
  const double discount = *((const double *)(row + discount_field_offset));
  const double quantity = *((const double *)(row + quantity_field_offset));
  const std::string comment = string_ncopy(row + comment_field_offset,
      comment_field_length);

  if (projection) {
    std::cout << order_key <<
      "|" << line_number <<
      std::endl;
  } else {
    std::cout << extended_price <<
      "|" << order_key <<
      "|" << line_number <<
      "|" << ship_date <<
      "|" << discount <<
      "|" << quantity <<
      "|" << comment <<
      std::endl;
  }

  print_lock.unlock();
}

static void print_fb(const char *fb, size_t fb_size, vector<Tables::col_info> &schema)
{
    if (quiet)
        return;

    print_lock.lock();
    Tables::printSkyFb(fb, fb_size, schema);
    print_lock.unlock();
}

static const size_t order_key_field_offset = 0;
static const size_t line_number_field_offset = 12;
static const size_t quantity_field_offset = 16;
static const size_t extended_price_field_offset = 24;
static const size_t discount_field_offset = 32;
static const size_t shipdate_field_offset = 50;
static const size_t comment_field_offset = 97;
static const size_t comment_field_length = 44;

static void worker_test_par(librados::IoCtx *ioctx, int i, uint64_t iters,
    bool test_par_read)
{
  std::stringstream ss;
  ss << "obj." << i;
  const std::string oid = ss.str();

  int ret = ioctx->create(oid, false);
  checkret(ret, 0);

  while (true) {
    ceph::bufferlist inbl, outbl;
    ::encode(iters, inbl);
    ::encode(test_par_read, inbl);
    ret = ioctx->exec(oid, "tabular", "test_par", inbl, outbl);
    checkret(ret, 0);
  }
}

static void worker_build_index(librados::IoCtx *ioctx)
{
  while (true) {
    work_lock.lock();
    if (target_objects.empty()) {
      work_lock.unlock();
      break;
    }
    std::string oid = target_objects.back();
    target_objects.pop_back();
    std::cout << "building index... " << oid << std::endl;
    work_lock.unlock();

    ceph::bufferlist inbl, outbl;
    ::encode(build_index_batch_size, inbl);
    int ret = ioctx->exec(oid, "tabular", "build_index", inbl, outbl);
    checkret(ret, 0);
  }
  ioctx->close();
}

// busy loop work to simulate high cpu cost ops
volatile uint64_t __tabular_x;
static void add_extra_row_cost(uint64_t cost)
{
  for (uint64_t i = 0; i < cost; i++) {
    __tabular_x += i;
  }
}

static void worker()
{
  std::unique_lock<std::mutex> lock(work_lock);
  while (true) {
    // wait for work, or done
    if (ready_ios.empty()) {
      if (stop)
        break;
      work_cond.wait(lock);
      continue;
    }

    // prepare result
    AioState *s = ready_ios.front();
    ready_ios.pop_front();

    // process result without lock. we own it now.
    lock.unlock();

    dispatch_lock.lock();
    outstanding_ios--;
    dispatch_lock.unlock();
    dispatch_cond.notify_one();

    struct timing times = s->times;
    uint64_t nrows_server_processed = 0;
    uint64_t eval2_start = getns();

    if (query == "flatbuf") {

        // librados read will return the original obj, which is a seq of bls.
        // cls read will return an obj with some stats and a seq of bls.

        bufferlist wrapped_bls;   // to store the seq of bls.

        // first extract the top-level statistics encoded during cls processing
        if (use_cls) {
            try {
                ceph::bufferlist::iterator it = s->bl.begin();
                ::decode(times.read_ns, it);
                ::decode(times.eval_ns, it);
                ::decode(nrows_server_processed, it);
                ::decode(wrapped_bls, it);  // contains a seq of encoded bls.
            } catch (ceph::buffer::error&) {
                int decode_runquery_cls = 0;
                assert(decode_runquery_cls);
            }
            nrows_processed += nrows_server_processed;
        } else {
            wrapped_bls = s->bl;  // contains a seq of encoded bls.
        }
        delete s;  // we're done processing all of the bls contained within

        // decode and process each bl (contains 1 flatbuf) in a loop.
        ceph::bufferlist::iterator it = wrapped_bls.begin();
        while (it.get_remaining() > 0) {
            ceph::bufferlist bl;
            try {
                ::decode(bl, it);  // unpack the next bl (flatbuf)
            } catch (ceph::buffer::error&) {
                int decode_runquery_noncls = 0;
                assert(decode_runquery_noncls);
            }

            // get our data as contiguous bytes before accessing as flatbuf
            const char* fb = bl.c_str();
            size_t fb_size = bl.length();
            Tables::sky_root_header root = \
                    Tables::getSkyRootHeader(fb, fb_size);

            // local counter to accumulate nrows in all flatbuffers received.
            rows_returned += root.nrows;

            int ret = 0;
            if (use_cls) {
                /* Server side processing already done at this point.
                 * TODO: any further client-side processing required here,
                 * such as possibly aggregate global ops here (e.g.,count/sort)
                 * then convert rows to valid db tuple format.
                 */

                // server has already applied its predicates, so count all rows
                // here that pass after applying the remaining global ops.
                result_count += root.nrows;

                Tables::schema_vec schema_out;
                ret = getSchemaFromSchemaString(schema_out, query_schema_str);
                assert(ret!=Tables::TablesErrCodes::EmptySchema);
                assert(ret!=Tables::TablesErrCodes::BadColInfoFormat);
                print_fb(fb, fb_size, schema_out);
            } else {
                // perform any extra project/select/agg if needed.
                bool more_processing = false;
                const char *fb_out;
                size_t fb_out_size;
                Tables::schema_vec schema_out;
                nrows_processed += root.nrows;

                // set the input schema (current schema of the fb)
                Tables::schema_vec schema_in;
                ret = getSchemaFromSchemaString(schema_in, table_schema_str);
                assert(ret!=Tables::TablesErrCodes::EmptySchema); // TODO errs
                assert(ret!=Tables::TablesErrCodes::BadColInfoFormat);
                ret = getSchemaFromSchemaString(schema_out, query_schema_str);
                assert(ret!=Tables::TablesErrCodes::EmptySchema);
                assert(ret!=Tables::TablesErrCodes::BadColInfoFormat);

                // set the output schema
                if (projection) {
                    more_processing = true;
                }

                if (!more_processing) {  // nothing left to do here.
                    fb_out = fb;
                    fb_out_size = fb_size;
                    result_count += root.nrows;
                } else {
                    flatbuffers::FlatBufferBuilder flatbldr(1024); // pre-alloc
                    std::string errmsg;
                    bool processedOK = true;
                    ret = processSkyFb(flatbldr, schema_in, schema_out,
                                       fb, fb_size, errmsg);
                    if (ret != 0) {
                        processedOK = false;
                        std::cerr << "ERROR: run-query() processing flatbuf: "
                                  << errmsg << endl;
                        std::cerr << "ERROR: Tables::ErrCodes=" << ret << endl;
                        assert(processedOK);
                    }
                    fb_out = reinterpret_cast<char*>(
                            flatbldr.GetBufferPointer());
                    fb_out_size = flatbldr.GetSize();
                    Tables::sky_root_header skyroot = \
                            Tables::getSkyRootHeader(fb_out, fb_out_size);
                    result_count += skyroot.nrows;
                }
                print_fb(fb_out, fb_out_size, schema_out);
            }
        } // endloop of processing sequence of encoded bls

    } else {   // older processing code below

        ceph::bufferlist bl;
        // if it was a cls read, first unpack some of the cls processing stats
        if (use_cls) {
            try {
                ceph::bufferlist::iterator it = s->bl.begin();
                ::decode(times.read_ns, it);
                ::decode(times.eval_ns, it);
                ::decode(nrows_server_processed, it);
            } catch (ceph::buffer::error&) {
                int decode_runquery_cls = 0;
                assert(decode_runquery_cls);
            }
        } else {
            bl = s->bl;
        }

        // data is now all in bl
        delete s;

        // our older query processing code below...
        // apply the query
        size_t row_size;
        if (projection && use_cls)
          row_size = 8;
        else
          row_size = 141;
        const char *rows = bl.c_str();
        const size_t num_rows = bl.length() / row_size;
        rows_returned += num_rows;

        if (use_cls)
            nrows_processed += nrows_server_processed;
        else
            nrows_processed += num_rows;

        if (query == "a") {
          if (use_cls) {
            // if we are using cls then storage system returns the number of
            // matching rows rather than the actual rows. so we patch up the
            // results to the presentation of the results is correct.
            size_t matching_rows;
            ceph::bufferlist::iterator it = bl.begin();
            ::decode(matching_rows, it);
            result_count += matching_rows;
          } else {
            if (projection && use_cls) {
              result_count += num_rows;
            } else {
              for (size_t rid = 0; rid < num_rows; rid++) {
                const char *row = rows + rid * row_size;
                const char *vptr = row + extended_price_field_offset;
                const double val = *(const double*)vptr;
                if (val > extended_price) {
                  result_count++;
                  // when a predicate passes, add some extra work
                  add_extra_row_cost(extra_row_cost);
                }
              }
            }
          }
        } else if (query == "b") {
          if (projection && use_cls) {
            for (size_t rid = 0; rid < num_rows; rid++) {
              const char *row = rows + rid * row_size;
              print_row(row);
              result_count++;
            }
          } else {
            for (size_t rid = 0; rid < num_rows; rid++) {
              const char *row = rows + rid * row_size;
              const char *vptr = row + extended_price_field_offset;
              const double val = *(const double*)vptr;
              if (val > extended_price) {
                print_row(row);
                result_count++;
                add_extra_row_cost(extra_row_cost);
              }
            }
          }
        } else if (query == "c") {
          if (projection && use_cls) {
            for (size_t rid = 0; rid < num_rows; rid++) {
              const char *row = rows + rid * row_size;
              print_row(row);
              result_count++;
            }
          } else {
            for (size_t rid = 0; rid < num_rows; rid++) {
              const char *row = rows + rid * row_size;
              const char *vptr = row + extended_price_field_offset;
              const double val = *(const double*)vptr;
              if (val == extended_price) {
                print_row(row);
                result_count++;
                add_extra_row_cost(extra_row_cost);
              }
            }
          }
        } else if (query == "d") {
          if (projection && use_cls) {
            for (size_t rid = 0; rid < num_rows; rid++) {
              const char *row = rows + rid * row_size;
              print_row(row);
              result_count++;
            }
          } else {
            for (size_t rid = 0; rid < num_rows; rid++) {
              const char *row = rows + rid * row_size;
              const char *vptr = row + order_key_field_offset;
              const int order_key_val = *(const int*)vptr;
              if (order_key_val == order_key) {
                const char *vptr = row + line_number_field_offset;
                const int line_number_val = *(const int*)vptr;
                if (line_number_val == line_number) {
                  print_row(row);
                  result_count++;
                  add_extra_row_cost(extra_row_cost);
                }
              }
            }
          }
        } else if (query == "e") {
          if (projection && use_cls) {
            for (size_t rid = 0; rid < num_rows; rid++) {
              const char *row = rows + rid * row_size;
              print_row(row);
              result_count++;
            }
          } else {
            for (size_t rid = 0; rid < num_rows; rid++) {
              const char *row = rows + rid * row_size;

              const int shipdate_val = *((const int *)(row + shipdate_field_offset));
              if (shipdate_val >= ship_date_low && shipdate_val < ship_date_high) {
                const double discount_val = *((const double *)(row + discount_field_offset));
                if (discount_val > discount_low && discount_val < discount_high) {
                  const double quantity_val = *((const double *)(row + quantity_field_offset));
                  if (quantity_val < quantity) {
                    print_row(row);
                    result_count++;
                    add_extra_row_cost(extra_row_cost);
                  }
                }
              }
            }
          }
        } else if (query == "f") {
          if (projection && use_cls) {
            for (size_t rid = 0; rid < num_rows; rid++) {
              const char *row = rows + rid * row_size;
              print_row(row);
              result_count++;
            }
          } else {
            RE2 re(comment_regex);
            for (size_t rid = 0; rid < num_rows; rid++) {
              const char *row = rows + rid * row_size;
              const char *cptr = row + comment_field_offset;
              const std::string comment_val = string_ncopy(cptr,
                  comment_field_length);
              if (RE2::PartialMatch(comment_val, re)) {
                print_row(row);
                result_count++;
                add_extra_row_cost(extra_row_cost);
              }
            }
          }
        } else if (query == "fastpath") {
            for (size_t rid = 0; rid < num_rows; rid++) {
              const char *row = rows + rid * row_size;
              print_row(row);
              result_count++;
            }
        } else {
          assert(0);
        }
    }

    times.eval2_ns = getns() - eval2_start;

    lock.lock();
    timings.push_back(times);
  }
}

/*
 * 1. free up aio resources
 * 2. put io on work queue
 * 3. wake-up a worker
 */
static void handle_cb(librados::completion_t cb, void *arg)
{
  AioState *s = (AioState*)arg;
  s->times.response = getns();
  assert(s->c->get_return_value() >= 0);
  s->c->release();
  s->c = NULL;

  work_lock.lock();
  ready_ios.push_back(s);
  work_lock.unlock();

  work_cond.notify_one();
}

int main(int argc, char **argv)
{
  std::string pool;
  unsigned num_objs;
  int wthreads;
  bool build_index;
  std::string logfile;
  int qdepth;
  std::string dir;
  std::string projected_col_names_default = "*";
  std::string projected_col_names;  // provided by the client/user or default
  Tables::schema_vec current_schema;  // current schema of the table
  bool apply_predicates = false;  // TODO

  // TODO: get actual table name and schema from the db client
  int ret = getSchemaFromSchemaString(current_schema, Tables::lineitem_test_schema_string);

  po::options_description gen_opts("General options");
  gen_opts.add_options()
    ("help,h", "show help message")
    ("pool", po::value<std::string>(&pool)->required(), "pool")
    ("num-objs", po::value<unsigned>(&num_objs)->required(), "num objects")
    ("use-cls", po::bool_switch(&use_cls)->default_value(false), "use cls")
    ("quiet,q", po::bool_switch(&quiet)->default_value(false), "quiet")
    ("query", po::value<std::string>(&query)->required(), "query name")
    ("wthreads", po::value<int>(&wthreads)->default_value(1), "num threads")
    ("qdepth", po::value<int>(&qdepth)->default_value(1), "queue depth")
    ("build-index", po::bool_switch(&build_index)->default_value(false), "build index")
    ("use-index", po::bool_switch(&use_index)->default_value(false), "use index")
    ("projection", po::bool_switch(&projection)->default_value(false), "projection")
    ("build-index-batch-size", po::value<uint32_t>(&build_index_batch_size)->default_value(1000), "build index batch size")
    ("extra-row-cost", po::value<uint64_t>(&extra_row_cost)->default_value(0), "extra row cost")
    ("log-file", po::value<std::string>(&logfile)->default_value(""), "log file")
    ("dir", po::value<std::string>(&dir)->default_value("fwd"), "direction")
    // query parameters
    ("extended-price", po::value<double>(&extended_price)->default_value(0.0), "extended price")
    ("order-key", po::value<int>(&order_key)->default_value(0.0), "order key")
    ("line-number", po::value<int>(&line_number)->default_value(0.0), "line number")
    ("ship-date-low", po::value<int>(&ship_date_low)->default_value(-9999), "ship date low")
    ("ship-date-high", po::value<int>(&ship_date_high)->default_value(-9999), "ship date high")
    ("discount-low", po::value<double>(&discount_low)->default_value(-9999.0), "discount low")
    ("discount-high", po::value<double>(&discount_high)->default_value(-9999.0), "discount high")
    ("quantity", po::value<double>(&quantity)->default_value(0.0), "quantity")
    ("comment_regex", po::value<std::string>(&comment_regex)->default_value(""), "comment_regex")
    ("project-col-names", po::value<std::string>(&projected_col_names)->default_value(projected_col_names_default), "projected col names, as csv list")
  ;

  po::options_description all_opts("Allowed options");
  all_opts.add(gen_opts);

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, all_opts), vm);

  if (vm.count("help")) {
    std::cout << all_opts << std::endl;
    return 1;
  }

  po::notify(vm);

  assert(num_objs > 0);
  assert(wthreads > 0);
  assert(qdepth > 0);

  // connect to rados
  librados::Rados cluster;
  cluster.init(NULL);
  cluster.conf_read_file(NULL);
  ret = cluster.connect();
  checkret(ret, 0);

  // open pool
  librados::IoCtx ioctx;
  ret = cluster.ioctx_create(pool.c_str(), ioctx);
  checkret(ret, 0);

  timings.reserve(num_objs);

  // generate the names of the objects to process
  for (unsigned oidx = 0; oidx < num_objs; oidx++) {
    std::stringstream oid_ss;
    oid_ss << "obj." << oidx;
    const std::string oid = oid_ss.str();
    target_objects.push_back(oid);
  }

  if (dir == "fwd") {
    std::reverse(std::begin(target_objects),
        std::end(target_objects));
  } else if (dir == "bwd") {
    // initial order
  } else if (dir == "rnd") {
    std::random_shuffle(std::begin(target_objects),
        std::end(target_objects));
  } else {
    assert(0);
  }

  // build index for query "d"
  if (build_index) {
    std::vector<std::thread> threads;
    for (int i = 0; i < wthreads; i++) {
      auto ioctx = new librados::IoCtx;
      int ret = cluster.ioctx_create(pool.c_str(), *ioctx);
      checkret(ret, 0);
      threads.push_back(std::thread(worker_build_index, ioctx));
    }

    for (auto& thread : threads) {
      thread.join();
    }

    return 0;
  }

  /*
   * sanity check queries against provided parameters
   */
  if (query == "a") {

    assert(!use_index); // not supported
    assert(extended_price != 0.0);
    std::cout << "select count(*) from lineitem where l_extendedprice > "
      << extended_price << std::endl;

  } else if (query == "b") {

    assert(!use_index); // not supported
    assert(extended_price != 0.0);
    std::cout << "select * from lineitem where l_extendedprice > "
      << extended_price << std::endl;

  } else if (query == "c") {

    assert(!use_index); // not supported
    assert(extended_price != 0.0);
    std::cout << "select * from lineitem where l_extendedprice = "
      << extended_price << std::endl;

  } else if (query == "d") {

    if (use_index)
      assert(use_cls);

    assert(order_key != 0);
    assert(line_number != 0);
    std::cout << "select * from from lineitem where l_orderkey = "
      << order_key << " and l_linenumber = " << line_number << std::endl;

  } else if (query == "e") {

    assert(!use_index); // not supported
    assert(ship_date_low != -9999);
    assert(ship_date_high != -9999);
    assert(discount_low != -9999.0);
    assert(discount_high != -9999.0);
    assert(quantity != 0.0);
    std::cout << "select * from lineitem where l_shipdate >= "
      << ship_date_low << " and l_shipdate < " << ship_date_high
      << " and l_discount > " << discount_low << " and l_discount < "
      << discount_high << " and l_quantity < " << quantity << std::endl;

  } else if (query == "f") {

    assert(!use_index); // not supported
    assert(comment_regex != "");
    std::cout << "select * from lineitem where l_comment ilike '%"
      << comment_regex << "%'" << std::endl;

  } else if (query == "fastpath") {   // no processing required

    assert(!use_index); // not supported
    assert(!projection); // not supported
    std::cout << "select * from lineitem" << std::endl;

  } else if (query == "flatbuf") {   // no processing required

    // the queryop schema string will be either the full current schema or projected schema.
    // set the query schema and check if proj&select

    Tables::schema_vec query_schema;
    boost::trim(projected_col_names);

    if (projected_col_names == projected_col_names_default) {

        // the query schema is identical to the current schema
        for (auto it=current_schema.begin(); it!=current_schema.end(); ++it)
            query_schema.push_back(*it);

        // treat as fastpath query, only if no project and no select
        if (!apply_predicates)
            fastpath = true;

    } else {
        projection = true;
        Tables::getSchemaFromProjectCols(query_schema, current_schema, projected_col_names);
        assert(query_schema.size() != 0);
    }

    table_schema_str = getSchemaStrFromSchema(current_schema);
    query_schema_str = getSchemaStrFromSchema(query_schema);

    std::cout << "select " << projected_col_names << " from lineitem" << std::endl;
    cout << "table_schema_str=\n" << table_schema_str << endl;
    cout << "query_schema_str=\n" << query_schema_str << endl;

  } else {
    std::cerr << "invalid query: " << query << std::endl;
    exit(1);
  }

  result_count = 0;
  rows_returned = 0;
  nrows_processed = 0;
  fastpath |= false;

  outstanding_ios = 0;
  stop = false;

  // start worker threads
  std::vector<std::thread> threads;
  for (int i = 0; i < wthreads; i++) {
    threads.push_back(std::thread(worker));
  }

  std::unique_lock<std::mutex> lock(dispatch_lock);
  while (true) {
    while (outstanding_ios < qdepth) {
      // get an object to process
      if (target_objects.empty())
        break;
      std::string oid = target_objects.back();
      target_objects.pop_back();
      lock.unlock();

      // dispatch an io request
      AioState *s = new AioState;
      s->c = librados::Rados::aio_create_completion(
          s, NULL, handle_cb);

      memset(&s->times, 0, sizeof(s->times));
      s->times.dispatch = getns();

      if (use_cls) {
        query_op op;
        op.query = query;
        op.extended_price = extended_price;
        op.order_key = order_key;
        op.line_number = line_number;
        op.ship_date_low = ship_date_low;
        op.ship_date_high = ship_date_high;
        op.discount_low = discount_low;
        op.discount_high = discount_high;
        op.quantity = quantity;
        op.comment_regex = comment_regex;
        op.use_index = use_index;
        op.projection = projection;
        op.fastpath = fastpath;

        // this is set above during user input err checking
        op.table_schema_str = table_schema_str;
        op.query_schema_str = query_schema_str;

        op.extra_row_cost = extra_row_cost;
        ceph::bufferlist inbl;
        ::encode(op, inbl);
        int ret = ioctx.aio_exec(oid, s->c,
            "tabular", "query_op", inbl, &s->bl);
        checkret(ret, 0);
      } else {
        int ret = ioctx.aio_read(oid, s->c, &s->bl, 0, 0);
        checkret(ret, 0);
      }

      lock.lock();
      outstanding_ios++;
    }
    if (target_objects.empty())
      break;
    dispatch_cond.wait(lock);
  }
  lock.unlock();

  // drain any still-in-flight operations
  while (true) {
    lock.lock();
    if (outstanding_ios == 0) {
      lock.unlock();
      break;
    }
    lock.unlock();
    std::cout << "draining ios: " << outstanding_ios << " remaining" << std::endl;
    sleep(1);
  }

  // wait for all the workers to stop
  work_lock.lock();
  stop = true;
  work_lock.unlock();
  work_cond.notify_all();

  // the threads will exit when all the objects are processed
  for (auto& thread : threads) {
    thread.join();
  }

  ioctx.close();

  if (query == "a" && use_cls) {
    std::cout << "total result row count: " << result_count
      << " / -1" << "; nrows_processed=" << nrows_processed
      << std::endl;
  } else {
    std::cout << "total result row count: " << result_count
      << " / " << rows_returned  << "; nrows_processed=" << nrows_processed
      << std::endl;
  }

  if (logfile.length()) {
    std::ofstream out;
    out.open(logfile, std::ios::trunc);
    out << "dispatch,response,read_ns,eval_ns,eval2_ns" << std::endl;
    for (const auto& time : timings) {
      out <<
        time.dispatch << "," <<
        time.response << "," <<
        time.read_ns << "," <<
        time.eval_ns << "," <<
        time.eval2_ns << std::endl;
    }
    out.close();
  }

  return 0;
}