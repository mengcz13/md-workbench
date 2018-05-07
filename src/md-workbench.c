// This file is part of MD-REAL-IO.
//
// MD-REAL-IO is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// MD-REAL-IO is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with MD-REAL-IO.  If not, see <http://www.gnu.org/licenses/>.
//
// Author: Julian Kunkel

#include <mpi.h>


#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <md_util.h>
#include <md_option.h>

#include <plugins/md-plugin.h>

#include <plugins/md-dummy.h>
#include <plugins/md-posix.h>
#include <plugins/md-posix-direct.h>
#include <plugins/md-postgres.h>
#include <plugins/md-mongo.h>
#include <plugins/md-s3.h>
#include <plugins/md-mpi.h>

struct md_plugin * md_plugin_list[] = {
& md_plugin_dummy,
& md_plugin_posix,
& md_plugin_posix_direct,
#ifdef MD_PLUGIN_MPIIO
& md_plugin_mpi,
#endif
#ifdef MD_PLUGIN_POSTGRES
& md_plugin_postgres,
#endif
#ifdef MD_PLUGIN_MONGO
& md_plugin_mongo,
#endif
#ifdef MD_PLUGIN_S3
& md_plugin_s3,
#endif
NULL
};

#define xstr(s) str(s)
#define str(s) #s

#ifndef VERSION
#ifdef GIT_BRANCH
#define VERSION xstr(GIT_COMMIT_HASH)"@"xstr(GIT_BRANCH)
#else
#define VERSION "UNKNOWN"
#endif
#endif

// successfull, errors
typedef struct {
  int suc;
  int err;
} op_stat_t;

// A runtime for an operation and when the operation was started
typedef struct{
  float time_since_app_start;
  float runtime;
} time_result_t;

typedef struct{
  float min;
  float q1;
  float median;
  float q3;
  float q90;
  float q99;
  float max;
} time_statistics_t;

// statistics for running a single phase
typedef struct{ // NOTE: if this type is changed, adjust end_phase() !!!
  double t; // maximum time
  double * t_all;

  op_stat_t dset_name;
  op_stat_t dset_create;
  op_stat_t dset_delete;

  op_stat_t obj_name;
  op_stat_t obj_create;
  op_stat_t obj_read;
  op_stat_t obj_stat;
  op_stat_t obj_delete;

  // time measurements individual runs
  uint64_t repeats;
  time_result_t * time_create;
  time_result_t * time_read;
  time_result_t * time_stat;
  time_result_t * time_delete;

  time_statistics_t stats_create;
  time_statistics_t stats_read;
  time_statistics_t stats_stat;
  time_statistics_t stats_delete;

  // the maximum time for any single operation
  double max_op_time;
  timer phase_start_timer;
  int stonewall_iterations;
} phase_stat_t;

#define CHECK_MPI_RET(ret) if (ret != MPI_SUCCESS){ printf("Unexpected error in MPI on Line %d\n", __LINE__);}
#define LLU (long long unsigned)
#define min(a,b) (a < b ? a : b)

struct benchmark_options{
  struct md_plugin * plugin;

  char * interface;
  int num;
  int precreate;
  int dset_count;

  int offset;
  int iterations;
  int file_size;
  int read_only;
  int stonewall_timer;
  int stonewall_timer_wear_out;

  char * latency_file_prefix;
  int latency_keep_all;

  int phase_cleanup;
  int phase_precreate;
  int phase_benchmark;

  int limit_memory;
  int limit_memory_between_phases;

  int verbosity;
  int process_report;

  int print_detailed_stats;
  int quiet_output;

  char * run_info_file;

  int ignore_precreate_errors;
  int rank;
  int size;

  float relative_waiting_factor;
  int adaptive_waiting_mode;
};

static int global_iteration = 0;

struct benchmark_options o;

void init_options(){
  memset(& o, 0, sizeof(o));
  o.interface = "posix";
  o.num = 1000;
  o.precreate = 3000;
  o.dset_count = 10;
  o.offset = 1;
  o.iterations = 3;
  o.file_size = 3901;
  o.run_info_file = "mdtest.status";
}

static void wait(double runtime){
  double waittime = runtime * o.relative_waiting_factor;
  //printf("waittime: %e\n", waittime);
  if(waittime < 0.01){
    timer start;
    start_timer(& start);
    double end = stop_timer(start) + waittime;
    double cur = stop_timer(start);
    while (cur < end){
      cur = stop_timer(start);
    }
  }else{
    struct timespec w;
    w.tv_sec = (time_t) (waittime);
    w.tv_nsec = (long) ((waittime - w.tv_sec) * 1000 * 1000 * 1000);
    nanosleep(& w, NULL);
  }
}

static void init_stats(phase_stat_t * p, size_t repeats){
  memset(p, 0, sizeof(phase_stat_t));
  p->repeats = repeats;
  size_t timer_size = repeats * sizeof(time_result_t);
  p->time_create = (time_result_t *) malloc(timer_size);
  p->time_read = (time_result_t *) malloc(timer_size);
  p->time_stat = (time_result_t *) malloc(timer_size);
  p->time_delete = (time_result_t *) malloc(timer_size);
}

static float add_timed_result(timer start, timer phase_start_timer, time_result_t * results, size_t pos, double * max_time, double * out_op_time){
  float curtime = timer_subtract(start, phase_start_timer);
  double op_time = stop_timer(start);
  results[pos].runtime = (float) op_time;
  results[pos].time_since_app_start = curtime;
  if (op_time > *max_time){
    *max_time = op_time;
  }
  *out_op_time = op_time;
  return curtime;
}

static void print_detailed_stat_header(){
    printf("phase\t\td name\tcreate\tdelete\tob nam\tcreate\tread\tstat\tdelete\tt_inc_b\tt_no_bar\tthp\tmax_t\n");
}

static int sum_err(phase_stat_t * p){
  return p->dset_name.err + p->dset_create.err +  p->dset_delete.err + p->obj_name.err + p->obj_create.err + p->obj_read.err + p->obj_stat.err + p->obj_delete.err;
}

static double statistics_mean(int count, double * arr){
  double sum = 0;
  for(int i=0; i < o.size; i++){
    sum += arr[i];
  }
  return sum / o.size;
}

static double statistics_std_dev(int count, double * arr){
  double mean = statistics_mean(count, arr);
  double sum = 0;
  for(int i=0; i < o.size; i++){
    sum += (mean - arr[i])*(mean - arr[i]);
  }
  return sqrt(sum / (o.size-1));
}

static void statistics_minmax(int count, double * arr, double * out_min, double * out_max){
  double min = 1e308;
  double max = 0;
  for(int i=0; i < o.size; i++){
    min = (arr[i] < min) ? arr[i] : min;
    max = (arr[i] > max) ? arr[i] : max;
  }
  *out_min = min;
  *out_max = max;
}

static void print_p_stat(char * buff, const char * name, phase_stat_t * p, double t, int print_global){
  const double tp = (double)(p->obj_create.suc + p->obj_read.suc) * o.file_size / t / 1024 / 1024;

  const int errs = sum_err(p);
  double r_min = 0;
  double r_max = 0;
  double r_mean = 0;
  double r_std = 0;

  if(p->t_all){
    // we can compute several derived values that provide insight about quality of service, latency distribution and load balancing
    statistics_minmax(o.size, p->t_all, & r_min, & r_max);
    r_mean = statistics_mean(o.size, p->t_all);
    r_std = statistics_std_dev(o.size, p->t_all);
  }

  if (o.print_detailed_stats){
    sprintf(buff, "%s \t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.3fs\t%.3fs\t%.2f MiB/s %.4e", name, p->dset_name.suc, p->dset_create.suc,  p->dset_delete.suc, p->obj_name.suc, p->obj_create.suc, p->obj_read.suc,  p->obj_stat.suc, p->obj_delete.suc, p->t, t, tp, p->max_op_time);

    if (errs > 0){
      sprintf(buff, "%s err\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d", name, p->dset_name.err, p->dset_create.err,  p->dset_delete.err, p->obj_name.err, p->obj_create.err, p->obj_read.err, p->obj_stat.err, p->obj_delete.err);
    }
  }else{
    int pos = 0;
    // single line
    pos += sprintf(buff, "%s process max:%.1fs ", name, t);
    if(print_global){
      pos += sprintf(buff + pos, "min:%.1fs mean: %.1fs balance:%.1f stddev:%.1f ", r_min, r_mean, r_min/r_max * 100.0, r_std);
    }
    int ioops_per_iter = 4;
    if(o.read_only){
      ioops_per_iter = 2;
    }

    switch(name[0]){
      case('b'):
        pos += sprintf(buff + pos, "rate:%.1f iops/s objects:%d rate:%.1f obj/s tp:%.1f MiB/s op-max:%.4es",
          p->obj_read.suc * ioops_per_iter / t, // write, stat, read, delete
          p->obj_read.suc,
          p->obj_read.suc / t,
          tp,
          p->max_op_time);

        if(o.relative_waiting_factor > 1e-9){
          pos += sprintf(buff + pos, " waiting_factor:%.2f", o.relative_waiting_factor);
        }
        break;
      case('p'):
        pos += sprintf(buff + pos, "rate:%.1f iops/s dsets: %d objects:%d rate:%.3f dset/s rate:%.1f obj/s tp:%.1f MiB/s op-max:%.4es",
          (p->dset_create.suc + p->obj_create.suc) / t,
          p->dset_create.suc,
          p->obj_create.suc,
          p->dset_create.suc / t,
          p->obj_create.suc / t,
          tp,
          p->max_op_time);
        break;
      case('c'):
        pos += sprintf(buff + pos, "rate:%.1f iops/s objects:%d dsets: %d rate:%.1f obj/s rate:%.3f dset/s op-max:%.4es",
          (p->obj_delete.suc + p->dset_delete.suc) / t,
          p->obj_delete.suc,
          p->dset_delete.suc,
          p->obj_delete.suc / t,
          p->dset_delete.suc / t,
          p->max_op_time);
        break;
      default:
        pos = sprintf(buff, "%s: unknown phase", name);
      break;
    }

    if(! o.quiet_output || errs > 0){
      pos += sprintf(buff + pos, " (%d errs", errs);
      if(errs > 0){
        pos += sprintf(buff + pos, "!!!)" );
      }else{
        pos += sprintf(buff + pos, ")" );
      }
    }
    if(! o.quiet_output && p->stonewall_iterations){
      pos += sprintf(buff + pos, " stonewall-iter:%d", p->stonewall_iterations);
    }

    if(p->stats_read.max > 1e-9){
      time_statistics_t stat = p->stats_read;
      pos += sprintf(buff + pos, " read(%.4es, %.4es, %.4es, %.4es, %.4es, %.4es, %.4es)", stat.min, stat.q1, stat.median, stat.q3, stat.q90, stat.q99, stat.max);
    }
    if(p->stats_stat.max > 1e-9){
      time_statistics_t stat = p->stats_stat;
      pos += sprintf(buff + pos, " stat(%.4es, %.4es, %.4es, %.4es, %.4es, %.4es, %.4es)", stat.min, stat.q1, stat.median, stat.q3, stat.q90, stat.q99, stat.max);
    }
    if(p->stats_create.max > 1e-9){
      time_statistics_t  stat = p->stats_create;
      pos += sprintf(buff + pos, " create(%.4es, %.4es, %.4es, %.4es, %.4es, %.4es, %.4es)", stat.min, stat.q1, stat.median, stat.q3, stat.q90, stat.q99, stat.max);
    }
    if(p->stats_delete.max > 1e-9){
      time_statistics_t stat = p->stats_delete;
      pos += sprintf(buff + pos, " delete(%.4es, %.4es, %.4es, %.4es, %.4es, %.4es, %.4es)", stat.min, stat.q1, stat.median, stat.q3, stat.q90, stat.q99, stat.max);
    }
  }
}

static int compare_floats(time_result_t * x, time_result_t * y){
  return x->runtime < y->runtime ? -1 : (x->runtime > y->runtime ? +1 : 0);
}

static double runtime_quantile(int repeats, time_result_t * times, float quantile){
  int pos = round(quantile * repeats + 0.49);
  return times[pos].runtime;
}

static uint64_t aggregate_timers(int repeats, int max_repeats, time_result_t * times, time_result_t * global_times){
  uint64_t count = 0;
  int ret;
  // due to stonewall, the number of repeats may be different per process
  if(o.rank == 0){
    MPI_Status status;
    memcpy(global_times, times, repeats * 2 * sizeof(float));
    count += repeats;
    for(int i=1; i < o.size; i++){
      int cnt;
      ret = MPI_Recv(& global_times[count], max_repeats*2, MPI_FLOAT, i, 888, MPI_COMM_WORLD, & status);
      CHECK_MPI_RET(ret)
      MPI_Get_count(& status, MPI_FLOAT, & cnt);
      count += cnt / 2;
    }
  }else{
    ret = MPI_Send(times, repeats * 2, MPI_FLOAT, 0, 888, MPI_COMM_WORLD);
    CHECK_MPI_RET(ret)
  }

  return count;
}

static void compute_histogram(const char * name, time_result_t * times, time_statistics_t * stats, size_t repeats, int writeLatencyFile){
  if(writeLatencyFile && o.latency_file_prefix ){
    char file[1024];
    sprintf(file, "%s-%.2f-%d-%s.csv", o.latency_file_prefix, o.relative_waiting_factor, global_iteration, name);
    FILE * f = fopen(file, "w+");
    if(f == NULL){
      printf("%d: Error writing to latency file: %s\n", o.rank, file);
      return;
    }
    fprintf(f, "time,runtime\n");
    for(size_t i = 0; i < repeats; i++){
      fprintf(f, "%.7f,%.4e\n", times[i].time_since_app_start, times[i].runtime);
    }
    fclose(f);
  }
  // now sort the times and pick the quantiles
  qsort(times, repeats, sizeof(time_result_t), (int (*)(const void *, const void *)) compare_floats);
  stats->min = times[0].runtime;
  stats->q1 = runtime_quantile(repeats, times, 0.25);
  if(repeats % 2 == 0){
    stats->median = (times[repeats/2].runtime + times[repeats/2 - 1].runtime)/2.0;
  }else{
    stats->median = times[repeats/2].runtime;
  }
  stats->q3 = runtime_quantile(repeats, times, 0.75);
  stats->q90 = runtime_quantile(repeats, times, 0.90);
  stats->q99 = runtime_quantile(repeats, times, 0.99);
  stats->max = times[repeats - 1].runtime;
}

static void end_phase(const char * name, phase_stat_t * p){
  int ret;
  char buff[4096];

  char * limit_memory_P = NULL;
  MPI_Barrier(MPI_COMM_WORLD);

  int max_repeats = o.precreate * o.dset_count;
  if(strcmp(name,"benchmark") == 0){
    max_repeats = o.num * o.dset_count;
  }

  // prepare the summarized report
  phase_stat_t g_stat;
  init_stats(& g_stat, (o.rank == 0 ? 1 : 0) * ((size_t) max_repeats) * o.size);
  // reduce timers
  ret = MPI_Reduce(& p->t, & g_stat.t, 2, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
  CHECK_MPI_RET(ret)
  if(o.rank == 0) {
    g_stat.t_all = (double*) malloc(sizeof(double) * o.size);
  }
  ret = MPI_Gather(& p->t, 1, MPI_DOUBLE, g_stat.t_all, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
  CHECK_MPI_RET(ret)
  ret = MPI_Reduce(& p->dset_name, & g_stat.dset_name, 2*(3+5), MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
  CHECK_MPI_RET(ret)
  ret = MPI_Reduce(& p->max_op_time, & g_stat.max_op_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
  CHECK_MPI_RET(ret)
  if( p->stonewall_iterations ){
    ret = MPI_Reduce(& p->repeats, & g_stat.repeats, 1, MPI_UINT64_T, MPI_MIN, 0, MPI_COMM_WORLD);
    CHECK_MPI_RET(ret)
    g_stat.stonewall_iterations = p->stonewall_iterations;
  }
  int write_rank0_latency_file = (o.rank == 0) && ! o.latency_keep_all;

  if(strcmp(name,"precreate") == 0){
    uint64_t repeats = aggregate_timers(p->repeats, max_repeats, p->time_create, g_stat.time_create);
    if(o.rank == 0){
      compute_histogram("precreate-all", g_stat.time_create, & g_stat.stats_create, repeats, o.latency_keep_all);
    }
    compute_histogram("precreate", p->time_create, & p->stats_create, p->repeats, write_rank0_latency_file);
  }else if(strcmp(name,"cleanup") == 0){
    uint64_t repeats = aggregate_timers(p->repeats, max_repeats, p->time_delete, g_stat.time_delete);
    if(o.rank == 0) {
      compute_histogram("cleanup-all", g_stat.time_delete, & g_stat.stats_delete, repeats, o.latency_keep_all);
    }
    compute_histogram("cleanup", p->time_delete, & p->stats_delete, p->repeats, write_rank0_latency_file);
  }else if(strcmp(name,"benchmark") == 0){
    uint64_t repeats = aggregate_timers(p->repeats, max_repeats, p->time_read, g_stat.time_read);
    if(o.rank == 0) {
      compute_histogram("read-all", g_stat.time_read, & g_stat.stats_read, repeats, o.latency_keep_all);
    }
    compute_histogram("read", p->time_read, & p->stats_read, p->repeats, write_rank0_latency_file);

    repeats = aggregate_timers(p->repeats, max_repeats, p->time_stat, g_stat.time_stat);
    if(o.rank == 0) {
      compute_histogram("stat-all", g_stat.time_stat, & g_stat.stats_stat, repeats, o.latency_keep_all);
    }
    compute_histogram("stat", p->time_stat, & p->stats_stat, p->repeats, write_rank0_latency_file);

    if(! o.read_only){
      repeats = aggregate_timers(p->repeats, max_repeats, p->time_create, g_stat.time_create);
      if(o.rank == 0) {
        compute_histogram("create-all", g_stat.time_create, & g_stat.stats_create, repeats, o.latency_keep_all);
      }
      compute_histogram("create", p->time_create, & p->stats_create, p->repeats, write_rank0_latency_file);

      repeats = aggregate_timers(p->repeats, max_repeats, p->time_delete, g_stat.time_delete);
      if(o.rank == 0) {
        compute_histogram("delete-all", g_stat.time_delete, & g_stat.stats_delete, repeats, o.latency_keep_all);
      }
      compute_histogram("delete", p->time_delete, & p->stats_delete, p->repeats, write_rank0_latency_file);
    }
  }

  if (o.rank == 0){
    //print the stats:
    print_p_stat(buff, name, & g_stat, g_stat.t, 1);
    printf("%s\n", buff);
  }

  if(o.process_report){
    if(o.rank == 0){
      print_p_stat(buff, name, p, p->t, 0);
      printf("0: %s\n", buff);
      for(int i=1; i < o.size; i++){
        MPI_Recv(buff, 4096, MPI_CHAR, i, 4711, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        printf("%d: %s\n", i, buff);
      }
    }else{
      print_p_stat(buff, name, p, p->t, 0);
      MPI_Send(buff, 4096, MPI_CHAR, 0, 4711, MPI_COMM_WORLD);
    }
  }

  if(g_stat.t_all){
    free(g_stat.t_all);
  }
  if(p->time_create){
    free(p->time_create);
    free(p->time_read);
    free(p->time_stat);
    free(p->time_delete);
  }
  if(g_stat.time_create){
    free(g_stat.time_create);
    free(g_stat.time_read);
    free(g_stat.time_stat);
    free(g_stat.time_delete);
  }

  // allocate if necessary
  ret = mem_preallocate(& limit_memory_P, o.limit_memory_between_phases, o.verbosity >= 3);
  if( ret != 0){
    printf("%d: Error allocating memory!\n", o.rank);
  }
  mem_free_preallocated(& limit_memory_P);
}

void run_precreate(phase_stat_t * s){
  char dset[4096];
  char obj_name[4096];
  int ret;

  for(int i=0; i < o.dset_count; i++){
    ret = o.plugin->def_dset_name(dset, o.rank, i);
    if (ret != MD_SUCCESS){
      if (! o.ignore_precreate_errors){
        printf("Error defining the dataset name\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
      }
      s->dset_name.err++;
      continue;
    }
    s->dset_name.suc++;
    ret = o.plugin->create_dset(dset);
    if (ret == MD_NOOP){
      // do not increment any counter
    }else if (ret == MD_SUCCESS){
      s->dset_create.suc++;
    }else{
      s->dset_create.err++;
      if (! o.ignore_precreate_errors){
        printf("%d: Error while creating the dset: %s\n", o.rank, dset);
        MPI_Abort(MPI_COMM_WORLD, 1);
      }
    }
  }

  char * buf = malloc(o.file_size);
  memset(buf, o.rank % 256, o.file_size);
  timer op_timer; // timer for individual operations
  size_t pos = -1; // position inside the individual measurement array
  double op_time;

  // create the obj
  for(int f=0; f < o.precreate; f++){
    for(int d=0; d < o.dset_count; d++){
      ret = o.plugin->def_dset_name(dset, o.rank, d);
      pos++;
      ret = o.plugin->def_obj_name(obj_name, o.rank, d, f);
      if (ret != MD_SUCCESS){
        s->dset_name.err++;
        if (! o.ignore_precreate_errors){
          printf("%d: Error while creating the obj name\n", o.rank);
          MPI_Abort(MPI_COMM_WORLD, 1);
        }
        s->obj_name.err++;
        continue;
      }

      start_timer(& op_timer);
      ret = o.plugin->write_obj(dset, obj_name, buf, o.file_size);
      add_timed_result(op_timer, s->phase_start_timer, s->time_create, pos, & s->max_op_time, & op_time);

      if (o.verbosity >= 2){
        printf("%d: write %s:%s (%d)\n", o.rank, dset, obj_name, ret);
      }

      if (ret == MD_NOOP){
        // do not increment any counter
      }else if (ret == MD_SUCCESS){
        s->obj_create.suc++;
      }else{
        s->obj_create.err++;
        if (! o.ignore_precreate_errors){
          printf("%d: Error while creating the obj: %s\n", o.rank, obj_name);
          MPI_Abort(MPI_COMM_WORLD, 1);
        }
      }
    }
  }
  free(buf);
}

/* FIFO: create a new file, write to it. Then read from the first created file, delete it... */
void run_benchmark(phase_stat_t * s, int * current_index_p){
  char dset[4096];
  char obj_name[4096];
  int ret;
  char * buf = malloc(o.file_size);
  memset(buf, o.rank % 256, o.file_size);
  timer op_timer; // timer for individual operations
  size_t pos = -1; // position inside the individual measurement array
  int start_index = *current_index_p;
  int total_num = o.num;
  int armed_stone_wall = (o.stonewall_timer > 0);
  int f;
  double phase_allreduce_time = 0;

  for(f=0; f < total_num; f++){
    float bench_runtime = 0; // the time since start
    for(int d=0; d < o.dset_count; d++){
      double op_time;
      const int prevFile = f + start_index;
      pos++;

      int readRank = (o.rank - o.offset * (d+1)) % o.size;
      readRank = readRank < 0 ? readRank + o.size : readRank;
      ret = o.plugin->def_obj_name(obj_name, readRank, d, prevFile);
      if (ret != MD_SUCCESS){
        s->obj_name.err++;
        continue;
      }
      ret = o.plugin->def_dset_name(dset, readRank, d);

      start_timer(& op_timer);
      ret = o.plugin->stat_obj(dset, obj_name, o.file_size);
      bench_runtime = add_timed_result(op_timer, s->phase_start_timer, s->time_stat, pos, & s->max_op_time, & op_time);
      if(o.relative_waiting_factor > 1e-9) {
        wait(op_time);
      }

      if (o.verbosity >= 2){
        printf("%d: stat %s:%s (%d)\n", o.rank, dset, obj_name, ret);
      }

      if(ret != MD_SUCCESS && ret != MD_NOOP){
        if (o.verbosity)
          printf("%d: Error while stating the obj: %s\n", o.rank, dset);
        s->obj_stat.err++;
        continue;
      }
      s->obj_stat.suc++;

      if (o.verbosity >= 2){
        printf("%d: read %s:%s \n", o.rank, dset, obj_name);
      }

      start_timer(& op_timer);
      ret = o.plugin->read_obj(dset, obj_name, buf, o.file_size);
      bench_runtime = add_timed_result(op_timer, s->phase_start_timer, s->time_read, pos, & s->max_op_time, & op_time);
      if(o.relative_waiting_factor > 1e-9) {
        wait(op_time);
      }

      if (ret == MD_SUCCESS){
        s->obj_read.suc++;
      }else if (ret == MD_NOOP){
        // nothing to do
      }else if (ret == MD_ERROR_FIND){
        printf("%d: Error while accessing the file %s (%s)\n", o.rank, dset, strerror(errno));
        s->obj_read.err++;
      }else{
        printf("%d: Error while reading the file %s (%s)\n", o.rank, dset, strerror(errno));
        s->obj_read.err++;
      }

      if(o.read_only){
        continue;
      }

      start_timer(& op_timer);
      ret = o.plugin->delete_obj(dset, obj_name);
      bench_runtime = add_timed_result(op_timer, s->phase_start_timer, s->time_delete, pos, & s->max_op_time, & op_time);
      if(o.relative_waiting_factor > 1e-9) {
        wait(op_time);
      }

      if (o.verbosity >= 2){
        printf("%d: delete %s:%s (%d)\n", o.rank, dset, obj_name, ret);
      }

      if (ret == MD_SUCCESS){
        s->obj_delete.suc++;
      }else if (ret == MD_NOOP){
        // nothing to do
      }else{
        printf("%d: Error while deleting the object %s:%s\n", o.rank, dset, obj_name);
        s->obj_delete.err++;
      }

      int writeRank = (o.rank + o.offset * (d+1)) % o.size;
      ret = o.plugin->def_obj_name(obj_name, writeRank, d, o.precreate + prevFile);
      if (ret != MD_SUCCESS){
        s->obj_name.err++;
        continue;
      }
      ret = o.plugin->def_dset_name(dset, writeRank, d);

      start_timer(& op_timer);
      ret = o.plugin->write_obj(dset, obj_name, buf, o.file_size);
      bench_runtime = add_timed_result(op_timer, s->phase_start_timer, s->time_create, pos, & s->max_op_time, & op_time);
      if(o.relative_waiting_factor > 1e-9) {
        wait(op_time);
      }

      if (o.verbosity >= 2){
        printf("%d: write %s:%s (%d)\n", o.rank, dset, obj_name, ret);
      }

      if (ret == MD_SUCCESS){
          s->obj_create.suc++;
      }else if (ret == MD_ERROR_CREATE){
        if (o.verbosity)
          printf("%d: Error while creating the obj: %s\n",o.rank, dset);
        s->obj_create.err++;
      }else if (ret == MD_NOOP){
          // do not increment any counter
      }else{
        if (o.verbosity)
          printf("%d: Error while writing the obj: %s\n", o.rank, dset);
        s->obj_create.err++;
      }
    } // end loop

    if(armed_stone_wall && bench_runtime >= o.stonewall_timer){
      if(o.verbosity){
        printf("%d: stonewall runtime %fs (%ds)\n", o.rank, bench_runtime, o.stonewall_timer);
      }
      if(! o.stonewall_timer_wear_out){
        s->stonewall_iterations = f;
        break;
      }
      armed_stone_wall = 0;
      // wear out mode, now reduce the maximum
      int cur_pos = f + 1;
      phase_allreduce_time = stop_timer(s->phase_start_timer);
      int ret = MPI_Allreduce(& cur_pos, & total_num, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
      start_timer(& s->phase_start_timer);
      CHECK_MPI_RET(ret)
      s->stonewall_iterations = total_num;
      if(o.rank == 0){
        printf("stonewall wear out %fs (%d iter)\n", bench_runtime, total_num);
      }
      if(f == total_num){
        break;
      }
    }
  }
  s->t = stop_timer(s->phase_start_timer) + phase_allreduce_time;
  if(armed_stone_wall && o.stonewall_timer_wear_out){
    int f = total_num;
    int ret = MPI_Allreduce(& f, & total_num, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    CHECK_MPI_RET(ret)
    s->stonewall_iterations = total_num;
  }
  if(o.stonewall_timer && ! o.stonewall_timer_wear_out){
    // TODO FIXME
    int sh = s->stonewall_iterations;
    int ret = MPI_Allreduce(& sh, & s->stonewall_iterations, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    CHECK_MPI_RET(ret)
  }

  if(! o.read_only) {
    *current_index_p += f;
  }
  s->repeats = pos + 1;
  free(buf);
}

void run_cleanup(phase_stat_t * s, int start_index){
  char dset[4096];
  char obj_name[4096];
  int ret;
  timer op_timer; // timer for individual operations
  size_t pos = -1; // position inside the individual measurement array

  for(int d=0; d < o.dset_count; d++){
    ret = o.plugin->def_dset_name(dset, o.rank, d);

    for(int f=0; f < o.precreate; f++){
      double op_time;
      pos++;
      ret = o.plugin->def_obj_name(obj_name, o.rank, d, f + start_index);

      start_timer(& op_timer);
      ret = o.plugin->delete_obj(dset, obj_name);
      add_timed_result(op_timer, s->phase_start_timer, s->time_delete, pos, & s->max_op_time, & op_time);

      if (o.verbosity >= 2){
        printf("%d: delete %s:%s (%d)\n", o.rank, dset, obj_name, ret);
      }

      if (ret == MD_NOOP){
        // nothing to do
      }else if (ret == MD_SUCCESS){
        s->obj_delete.suc++;
      }else if(ret != MD_NOOP){
        s->obj_delete.err++;
      }
    }

    ret = o.plugin->rm_dset(dset);

    if (o.verbosity >= 2){
      printf("%d: delete dset %s (%d)\n", o.rank, dset, ret);
    }

    if (ret == MD_SUCCESS){
      s->dset_delete.suc++;
    }else if (ret != MD_NOOP){
      s->dset_delete.err++;
    }
  }
}


static option_help options [] = {
  {'O', "offset", "Offset in o.ranks between writers and readers. Writers and readers should be located on different nodes.", OPTION_OPTIONAL_ARGUMENT, 'd', & o.offset},
  {'i', "interface", "The interface (plugin) to use for the test, use list to show all compiled plugins.", OPTION_OPTIONAL_ARGUMENT, 's', & o.interface},
  {'I', "obj-per-proc", "Number of I/O operations per data set.", OPTION_OPTIONAL_ARGUMENT, 'd', & o.num},
  {'L', "latency", "Measure the latency for individual operations, prefix the result files with the provided filename.", OPTION_OPTIONAL_ARGUMENT, 's', & o.latency_file_prefix},
  {0, "latency-all", "Keep the latency files from all ranks.", OPTION_FLAG, 'd', & o.latency_keep_all},
  {'P', "precreate-per-set", "Number of object to precreate per data set.", OPTION_OPTIONAL_ARGUMENT, 'd', & o.precreate},
  {'D', "data-sets", "Number of data sets covered per process and iteration.", OPTION_OPTIONAL_ARGUMENT, 'd', & o.dset_count},
  {'q', "quiet", "Avoid irrelevant printing.", OPTION_FLAG, 'd', & o.quiet_output},
  {'m', "lim-free-mem", "Allocate memory until this limit (in MiB) is reached.", OPTION_OPTIONAL_ARGUMENT, 'd', & o.limit_memory},
  {'M', "lim-free-mem-phase", "Allocate memory until this limit (in MiB) is reached between the phases, but free it before starting the next phase; the time is NOT included for the phase.", OPTION_OPTIONAL_ARGUMENT, 'd', & o.limit_memory_between_phases},
  {'S', "object-size", "Size for the created objects.", OPTION_OPTIONAL_ARGUMENT, 'd', & o.file_size},
  {'R', "iterations", "Number of times to rerun the main phase", OPTION_OPTIONAL_ARGUMENT, 'd', & o.iterations},
  {'t', "waiting-time", "Waiting time relative to runtime (1.0 is 100%%)", OPTION_OPTIONAL_ARGUMENT, 'f', & o.relative_waiting_factor},
  {'T', "adaptive-waiting", "Compute an adaptive waiting time", OPTION_FLAG, 'd', & o.adaptive_waiting_mode},
  {'1', "run-precreate", "Run precreate phase", OPTION_FLAG, 'd', & o.phase_precreate},
  {'2', "run-benchmark", "Run benchmark phase", OPTION_FLAG, 'd', & o.phase_benchmark},
  {'3', "run-cleanup", "Run cleanup phase (only run explicit phases)", OPTION_FLAG, 'd', & o.phase_cleanup},
  {'w', "stonewall-timer", "Stop each benchmark iteration after the specified seconds (if not used with -W this leads to process-specific progress!)", OPTION_OPTIONAL_ARGUMENT, 'd', & o.stonewall_timer},
  {'W', "stonewall-wear-out", "Stop with stonewall after specified time and use a soft wear-out phase -- all processes perform the same number of iterations", OPTION_FLAG, 'd', & o.stonewall_timer_wear_out},
  {0, "print-detailed-stats", "Print detailed machine parsable statistics.", OPTION_FLAG, 'd', & o.print_detailed_stats},
  {0, "read-only", "Run read-only during benchmarking phase (no deletes/writes), probably use with -2", OPTION_FLAG, 'd', & o.read_only},
  {0, "ignore-precreate-errors", "Ignore errors occuring during the pre-creation phase", OPTION_FLAG, 'd', & o.ignore_precreate_errors},
  {0, "process-reports", "Independent report per process/rank", OPTION_FLAG, 'd', & o.process_report},
  {'v', "verbose", "Increase the verbosity level", OPTION_FLAG, 'd', & o.verbosity},
  {0, "run-info-file", "The log file for resuming a previous run", OPTION_OPTIONAL_ARGUMENT, 's', & o.run_info_file},
  LAST_OPTION
  };

static void find_interface(){
  int is_list = strcmp(o.interface, "list") == 0 && o.rank == 0;
  if (is_list){
    printf("Available plugins: ");
  }
  struct md_plugin ** p_it = md_plugin_list;
  while(*p_it != NULL){
    if(is_list){
      printf("%s ", (*p_it)->name);
    }
    if((*p_it)->name == NULL){
      printf("Error, module \"%s\" not linked properly\n", o.interface);
      MPI_Abort(MPI_COMM_WORLD, 1);
    }
    if(strcmp((*p_it)->name, o.interface) == 0) {
      // got it
      o.plugin = *p_it;
      return;
    }
    p_it++;
  }
  if (o.rank == 0){
    if(is_list){
      printf("\n");
    }else{
      printf("Could not find plugin for interface: %s\n", o.interface);
      MPI_Abort(MPI_COMM_WORLD, 1);
    }
  }
  MPI_Finalize();
  exit(0);
}

static void printTime(){
    char buff[100];
    time_t now = time(0);
    strftime (buff, 100, "%Y-%m-%d %H:%M:%S", localtime (&now));
    printf("%s\n", buff);
}

static int return_position(){
  int position, ret;
  FILE * f = fopen(o.run_info_file, "r");
  if(! f){
    printf("[ERROR] Could not open %s for restart\n", o.run_info_file);
    exit(1);
  }
  ret = fscanf(f, "pos: %d", & position);
  if (ret != 1){
    printf("Could not read from %s for restart\n", o.run_info_file);
    exit(1);
  }
  fclose(f);
  return position;
}

static void store_position(int position){
  FILE * f = fopen(o.run_info_file, "w");
  if(! f){
    printf("[ERROR] Could not open %s for saving data\n", o.run_info_file);
    exit(1);
  }
  fprintf(f, "pos: %d\n", position);
  fclose(f);
}

int main(int argc, char ** argv){
  int ret;
  int printhelp = 0;
  char * limit_memory_P = NULL;

  init_options();

  MPI_Init(& argc, & argv);
  MPI_Comm_rank(MPI_COMM_WORLD, & o.rank);
  MPI_Comm_size(MPI_COMM_WORLD, & o.size);

  if (o.rank == 0 && ! o.quiet_output){
    printf("Args: %s", argv[0]);
    for(int i=1; i < argc; i++){
      printf(" \"%s\"", argv[i]);
    }
    printf("\n");
  }

  int parsed = parseOptions(argc, argv, options, & printhelp);

  find_interface();

  parseOptions(argc - parsed, argv + parsed, o.plugin->get_options(), & printhelp);

  if(printhelp != 0){
    if (o.rank == 0){
      printf("\nSynopsis: %s ", argv[0]);

      print_help(options, 0);

      printf("\nPlugin options for interface %s\n", o.interface);
      print_help(o.plugin->get_options(), 1);
    }
    MPI_Finalize();
    if(printhelp == 1){
      exit(0);
    }else{
      exit(1);
    }
  }

  if (!(o.phase_cleanup || o.phase_precreate || o.phase_benchmark)){
    // enable all phases
    o.phase_cleanup = o.phase_precreate = o.phase_benchmark = 1;
  }
  if (! o.phase_precreate && o.phase_benchmark && o.stonewall_timer && ! o.stonewall_timer_wear_out){
    if(o.rank == 0)
      printf("Invalid options, if running only the benchmark phase using -2 with stonewall option then use stonewall wear-out\n");
    exit(1);
  }

  ret = o.plugin->initialize();
  if (ret != MD_SUCCESS){
    printf("%d: Error initializing module\n", o.rank);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }

  int current_index = 0;
  if ( (o.phase_cleanup || o.phase_benchmark) && ! o.phase_precreate ){
    current_index = return_position();
  }

  size_t total_obj_count = o.dset_count * (size_t) (o.num * o.iterations + o.precreate) * o.size;
  if (o.rank == 0 && ! o.quiet_output){
    printf("MD-REAL-IO total objects: %zu workingset size: %.3f MiB (version: %s) time: ", total_obj_count, ((double) o.size) * o.dset_count * o.precreate * o.file_size / 1024.0 / 1024.0,  VERSION);
    printTime();
    if(o.num > o.precreate){
      printf("WARNING: num > precreate, this may cause the situation that no objects are available to read\n");
    }
  }

  if ( o.rank == 0 && ! o.quiet_output ){
    // print the set output options
    print_current_options(options);
    printf("\n");
    print_current_options(o.plugin->get_options());

    printf("\n");
  }

  // preallocate memory if necessary
  ret = mem_preallocate(& limit_memory_P, o.limit_memory, o.verbosity >= 3);
  if(ret != 0){
    printf("%d: Error allocating memory\n", o.rank);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }

  timer bench_start;
  start_timer(& bench_start);
  phase_stat_t phase_stats;

  if(o.rank == 0 && o.print_detailed_stats && ! o.quiet_output){
    print_detailed_stat_header();
  }

  if (o.phase_precreate){
    if (o.rank == 0){
      ret = o.plugin->prepare_global();
      if ( ret != MD_SUCCESS && ret != MD_NOOP ){
        printf("Rank 0 could not prepare the run, aborting\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
      }
    }
    init_stats(& phase_stats, o.precreate * o.dset_count);
    MPI_Barrier(MPI_COMM_WORLD);

    // pre-creation phase
    start_timer(& phase_stats.phase_start_timer);
    run_precreate(& phase_stats);
    phase_stats.t = stop_timer(phase_stats.phase_start_timer);
    end_phase("precreate", & phase_stats);
  }

  if (o.phase_benchmark){
    // benchmark phase
    for(global_iteration = 0; global_iteration < o.iterations; global_iteration++){
      if(o.adaptive_waiting_mode){
        o.relative_waiting_factor = 0;
      }
      init_stats(& phase_stats, o.num * o.dset_count);
      MPI_Barrier(MPI_COMM_WORLD);
      start_timer(& phase_stats.phase_start_timer);
      run_benchmark(& phase_stats, & current_index);
      end_phase("benchmark", & phase_stats);

      if(o.adaptive_waiting_mode){
        o.relative_waiting_factor = 0.0625;
        for(int r=0; r <= 6; r++){
          init_stats(& phase_stats, o.num * o.dset_count);
          MPI_Barrier(MPI_COMM_WORLD);
          start_timer(& phase_stats.phase_start_timer);
          run_benchmark(& phase_stats, & current_index);
          end_phase("benchmark", & phase_stats);
          o.relative_waiting_factor *= 2;
        }
      }
    }
  }

  // cleanup phase
  if (o.phase_cleanup){
    init_stats(& phase_stats, o.precreate * o.dset_count);
    start_timer(& phase_stats.phase_start_timer);
    run_cleanup(& phase_stats, current_index);
    phase_stats.t = stop_timer(phase_stats.phase_start_timer);
    end_phase("cleanup", & phase_stats);

    if (o.rank == 0){
      ret = o.plugin->purge_global();
      if (ret != MD_SUCCESS && ret != MD_NOOP){
        printf("Rank 0: Error purging the global environment\n");
      }
    }
  }else{
    store_position(current_index);
  }

  double t_all = stop_timer(bench_start);
  ret = o.plugin->finalize();
  if (ret != MD_SUCCESS){
    printf("Error while finalization of module\n");
  }
  if (o.rank == 0 && ! o.quiet_output){
    printf("Total runtime: %.0fs time: ",  t_all);
    printTime();
  }

  mem_free_preallocated(& limit_memory_P);

  MPI_Finalize();
  return 0;
}
