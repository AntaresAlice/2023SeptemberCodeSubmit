#include "bindex.h"

Timer timer;

int prefetch_stride = 6;

std::mutex scan_refine_mutex;
int scan_refine_in_position;
CODE scan_selected_compares[MAX_BINDEX_NUM][2];
bool scan_skip_refine;
// bool scan_use_special_compare;
bool scan_skip_other_face[MAX_BINDEX_NUM];
bool scan_skip_this_face[MAX_BINDEX_NUM];
CODE scan_max_compares[MAX_BINDEX_NUM][2];
bool scan_inverse_this_face[MAX_BINDEX_NUM];
char scan_cmd_file[256];

std::vector<std::string> stringSplit(const std::string& str, char delim) {
    std::string s;
    s.append(1, delim);
    std::regex reg(s);
    std::vector<std::string> elems(std::sregex_token_iterator(str.begin(), str.end(), reg, -1),
                                   std::sregex_token_iterator());
    return elems;
}

BITS gen_less_bits(const CODE *val, CODE compare, int n) {
  // n must <= BITSWIDTH (32)
  BITS result = 0;
  for (int i = 0; i < n; i++) {
    if (val[i] < compare) {
      result += (1 << (BITSWIDTH - 1 - i));
    }
  }
  return result;
}


void init_pos_block(pos_block *pb, CODE *val_f, POSTYPE *pos_f, int n) {
  assert(n <= blockInitSize);
  pb->length = n;
  pb->pos = (POSTYPE *)malloc(blockMaxSize * sizeof(POSTYPE));
  pb->val = (CODE *)malloc(blockMaxSize * sizeof(CODE));
  for (int i = 0; i < n; i++) {
    pb->pos[i] = pos_f[i];
    pb->val[i] = val_f[i];
  }
}

CODE block_start_value(pos_block *pb) { return pb->val[0]; }

int insert_to_block(pos_block *pb, CODE *val_f, POSTYPE *pos_f, int n) {
  // Insert max(n, #vacancy) elements to a block, return 0 if the block is still
  // not filled up.
  if (DEBUG_TIME_COUNT) timer.commonGetStartTime(2);
  int flagNum, length_new;
  if ((n + pb->length) >= blockMaxSize) {
    // Block filled up! This block will be splitted in insert_to_area(..)
    flagNum = blockMaxSize - pb->length;  // The number of successfully inserted
    // elements, flagNum will be return
    length_new = blockMaxSize;
  } else {
    flagNum = 0;
    length_new = pb->length + n;
  }

  int k, i, j;
  // Merge two sorted array
  for (k = length_new - 1, i = pb->length - 1, j = length_new - pb->length - 1; i >= 0 && j >= 0;) {
    if (val_f[j] >= pb->val[i]) {
      pb->val[k] = val_f[j];
      pb->pos[k--] = pos_f[j--];
    } else {
      pb->val[k] = pb->val[i];
      pb->pos[k--] = pb->pos[i--];
    }
  }
  while (j >= 0) {
    pb->val[k] = val_f[j];
    pb->pos[k--] = pos_f[j--];
  }
  pb->length = length_new;

  if (DEBUG_TIME_COUNT) timer.commonGetEndTime(2);
  return flagNum;
}

void init_area(Area *area, CODE *val, POSTYPE *pos, int n) {
  // Area containing only unique code should be considered in
  // future implementation
  int i = 0;
  area->blockNum = 0;
  area->length = n;
  while (i + blockInitSize < n) {
    area->blocks[area->blockNum] = (pos_block *)malloc(sizeof(pos_block));
    init_pos_block(area->blocks[area->blockNum], val + i, pos + i, blockInitSize);
    (area->blockNum)++;
    i += blockInitSize;
  }
  area->blocks[area->blockNum] = (pos_block *)malloc(sizeof(pos_block));
  init_pos_block(area->blocks[area->blockNum], val + i, pos + i, n - i);
  area->blockNum++;
  assert(area->blockNum <= blockNumMax);
}

CODE area_start_value(Area *area) { return area->blocks[0]->val[0]; }

void set_fv_val_less(BITS *bitmap, const CODE *val, CODE compare, POSTYPE n) {
  // Set values for filter vectors
  int i;
  for (i = 0; i + BITSWIDTH < (int)n; i += BITSWIDTH) {
    bitmap[i / BITSWIDTH] = gen_less_bits(val + i, compare, BITSWIDTH);
  }
  bitmap[i / BITSWIDTH] = gen_less_bits(val + i, compare, n - i);
}

inline POSTYPE num_insert_to_area(POSTYPE *areaStartIdx, int k, int n) {
  if (k < K - 1) {
    return areaStartIdx[k + 1] - areaStartIdx[k];
  } else {
    return n - areaStartIdx[k];
  }
}

void copy_block_to_GPU(pos_block *block_in_CPU, pos_block *block_in_GPU) {
  cudaError_t cudaStatus;

  block_in_GPU->length = block_in_CPU->length;

  cudaStatus = cudaMalloc((void**)&(block_in_GPU->pos), blockMaxSize * sizeof(POSTYPE));
  if (cudaStatus != cudaSuccess) {
    fprintf(stderr, "cudaMalloc pos block failed!");
    exit(-1);
  }

  cudaStatus = cudaMemcpy(block_in_GPU->pos, block_in_CPU->pos, blockMaxSize * sizeof(POSTYPE), cudaMemcpyHostToDevice);
  if (cudaStatus != cudaSuccess) {
    fprintf(stderr, "cudaCopy pos block failed!");
    exit(-1);
  }
}

void copy_area_to_GPU(Area *area_in_CPU, Area *area_in_GPU) {
  cudaError_t cudaStatus;

  area_in_GPU->blockNum = area_in_CPU->blockNum;
  area_in_GPU->length = area_in_CPU->length;
  for (int i = 0; i < area_in_CPU->blockNum; i++) {
    area_in_GPU->blocks[i] = (pos_block *)malloc(sizeof(pos_block));
    copy_block_to_GPU(area_in_CPU->blocks[i], area_in_GPU->blocks[i]);
  }
}

void start_timer(struct timeval* t) {
    gettimeofday(t, NULL);
}

void stop_timer(struct timeval* t, double* elapsed_time) {
    struct timeval end;
    gettimeofday(&end, NULL);
    *elapsed_time += (end.tv_sec - t->tv_sec) * 1000.0 + (end.tv_usec - t->tv_usec) / 1000.0;
}

void init_bindex_in_GPU(BinDex *bindex, CODE *data, POSTYPE n, size_t &base_data) {
  bindex->length = n;
  POSTYPE avgAreaSize = n / K;
  cudaError_t cudaStatus;

  // CODE areaStartValues[K];
  POSTYPE areaStartIdx[K];

  CODE *data_sorted = (CODE *)malloc(n * sizeof(CODE));  // Sorted codes
  double elapse_time = 0.0;
  timeval start;
  start_timer(&start);
  POSTYPE *pos = argsort(data, n);
  stop_timer(&start, &elapse_time);
  cout << "Sort time: " << elapse_time << endl;
  cerr << "Sort time: " << elapse_time << endl;
  
  elapse_time = 0.0;
  start_timer(&start);
  for (int i = 0; i < n; i++) {
    data_sorted[i] = data[pos[i]];
  }
  stop_timer(&start, &elapse_time);
  cout << "Assign time: " << elapse_time << endl;
  cerr << "Assign time: " << elapse_time << endl;
  
  size_t total, t1, t2;
  cudaMemGetInfo( &t1, &total );
  // copy raw_data to GPU
  cudaMalloc((void**)&(bindex->rawDataInGPU), n * sizeof(CODE));
  cudaMemcpy(bindex->rawDataInGPU, data, n * sizeof(CODE), cudaMemcpyHostToDevice);
  cudaMemGetInfo( &t2, &total );
  base_data += t1 - t2;

  bindex->data_min = data_sorted[0];
  bindex->data_max = data_sorted[bindex->length - 1];

  printf("Bindex data min: %u  max: %u\n", bindex->data_min, bindex->data_max);

  bindex->areaStartValues[0] = data_sorted[0];
  areaStartIdx[0] = 0;

  for (int i = 1; i < K; i++) {
    bindex->areaStartValues[i] = data_sorted[i * avgAreaSize];
    int j = i * avgAreaSize;
    if (bindex->areaStartValues[i] == bindex->areaStartValues[i - 1]) {
      areaStartIdx[i] = j;
    } else {
      // To find the first element which is less than startValue
      while (data_sorted[j] == bindex->areaStartValues[i]) {
        j--;
      }
      areaStartIdx[i] = j + 1;
    }
  }
  
  std::thread threads[THREAD_NUM];

  for (int k = 0; k * THREAD_NUM < K; k++) {
      for (int j = 0; j < THREAD_NUM && (k * THREAD_NUM + j) < K; j++) {
        int area_idx = k * THREAD_NUM + j;
        bindex->areas[area_idx] = (Area *)malloc(sizeof(Area));
        POSTYPE area_size = num_insert_to_area(areaStartIdx, area_idx, n);
        // printf("[area_size] %d\n", area_size);
        bindex->area_counts[area_idx] = area_size;
        threads[j] = std::thread(init_area, bindex->areas[area_idx], data_sorted + areaStartIdx[area_idx],
                                pos + areaStartIdx[area_idx], area_size);
      }
      for (int j = 0; j < THREAD_NUM && (k * THREAD_NUM + j) < K; j++) {
        threads[j].join();
      }
  }
  printf("[INFO] area initialized.\n");
  // Accumulative adding
  for (int i = 1; i < K; i++) {
    bindex->area_counts[i] += bindex->area_counts[i - 1];
  }
  assert(bindex->area_counts[K - 1] == bindex->length);

  // copy CPU areas to GPU areas
  for (int i = 0; i < K; i++) {
    bindex->areasInGPU[i] = (Area *)malloc(sizeof(Area));
    copy_area_to_GPU(bindex->areas[i], bindex->areasInGPU[i]);
  }
  printf("[INFO] copy areas to GPU done.\n");

  // Build the filterVectors
  // Now we build them in CPU memory and then copy them to GPU memory
  for (int k = 0; k * THREAD_NUM < K - 1; k++) {
    for (int j = 0; j < THREAD_NUM && (k * THREAD_NUM + j) < (K - 1); j++) {
      bindex->filterVectors[k * THREAD_NUM + j] =
          (BITS *)aligned_alloc(SIMD_ALIGEN, bits_num_needed(n) * sizeof(BITS));
      threads[j] = std::thread(set_fv_val_less, bindex->filterVectors[k * THREAD_NUM + j], data,
                               bindex->areaStartValues[k * THREAD_NUM + j + 1], n);
    }
    for (int j = 0; j < THREAD_NUM && (k * THREAD_NUM + j) < (K - 1); j++) {
      threads[j].join();
    }
  }

  printf("[INFO] build filterVectors done.\n");

  for (int i = 0; i < K - 1; i++) {
    cudaStatus = cudaMalloc((void**)&(bindex->filterVectorsInGPU[i]), bits_num_needed(n) * sizeof(BITS));
    if (cudaStatus != cudaSuccess) {
        fprintf(stderr, "cudaMalloc failed!");
        exit(-1);
    }

    cudaStatus = cudaMemcpy(bindex->filterVectorsInGPU[i], bindex->filterVectors[i], bits_num_needed(n) * sizeof(BITS), cudaMemcpyHostToDevice);
    if (cudaStatus != cudaSuccess) {
        fprintf(stderr, "cudaMemcpy failed!");
        exit(-1);
    }
  }

  free(pos);
  free(data_sorted);
}

void copy_bitmap(BITS *result, BITS *ref, int n, int t_id) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t_id * 2, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) < 0) {
    fprintf(stderr, "set thread affinity failed\n");
  }
  memcpy(result, ref, n * sizeof(BITS));

  // for (int i = 0; i < n; i++) {
  //     result[i] = ref[i];
  // }
}

void copy_bitmap_not(BITS *result, BITS *ref, int start_n, int end_n, int t_id) {
  int jobs = ROUNDUP_DIVIDE(end_n - start_n, THREAD_NUM);
  int start = start_n + t_id * jobs;
  int end = start_n + (t_id + 1) * jobs;
  if (end > end_n) end = end_n;
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t_id * 2, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) < 0) {
    fprintf(stderr, "set thread affinity failed\n");
  }

  for (int i = start; i < end; i++) {
    result[i] = ~(ref[i]);
  }
}

void copy_bitmap_bt(BITS *result, BITS *ref_l, BITS *ref_r, int start_n, int end_n, int t_id) {
  int jobs = ROUNDUP_DIVIDE(end_n - start_n, THREAD_NUM);
  int start = start_n + t_id * jobs;
  int end = start_n + (t_id + 1) * jobs;
  if (end > end_n) end = end_n;

  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t_id * 2, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) < 0) {
    fprintf(stderr, "set thread affinity failed\n");
  }

  for (int i = start; i < end; i++) {
    result[i] = ref_r[i] & (~(ref_l[i]));
  }
}

void copy_bitmap_bt_simd(BITS *to, BITS *from_l, BITS *from_r, int bitmap_len, int t_id) {
  int jobs = ((bitmap_len / SIMD_JOB_UNIT - 1) / THREAD_NUM + 1) * SIMD_JOB_UNIT;

  assert(jobs % SIMD_JOB_UNIT == 0);
  assert(bitmap_len % SIMD_JOB_UNIT == 0);
  assert(jobs * THREAD_NUM >= bitmap_len);

  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t_id * 2, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) < 0) {
    fprintf(stderr, "set thread affinity failed\n");
  }

  int cur = t_id * jobs;
  int end = (t_id + 1) * jobs;
  if (end > bitmap_len) end = bitmap_len;

  while (cur < end) {
    __m256i buf_l = _mm256_load_si256((__m256i *)(from_l + cur));
    __m256i buf_r = _mm256_load_si256((__m256i *)(from_r + cur));
    __m256i buf = _mm256_andnot_si256(buf_l, buf_r);
    _mm256_store_si256((__m256i *)(to + cur), buf);
    cur += SIMD_JOB_UNIT;
  }
}

void copy_bitmap_simd(BITS *to, BITS *from, int bitmap_len, int t_id) {
  int jobs = ((bitmap_len / SIMD_JOB_UNIT - 1) / THREAD_NUM + 1) * SIMD_JOB_UNIT;

  assert(jobs % SIMD_JOB_UNIT == 0);
  assert(bitmap_len % SIMD_JOB_UNIT == 0);
  assert(jobs * THREAD_NUM >= bitmap_len);

  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t_id * 2, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) < 0) {
    fprintf(stderr, "set thread affinity failed\n");
  }

  int cur = t_id * jobs;
  int end = (t_id + 1) * jobs;
  if (end > bitmap_len) end = bitmap_len;

  while (cur < end) {
    __m256i buf = _mm256_load_si256((__m256i *)(from + cur));
    _mm256_store_si256((__m256i *)(to + cur), buf);
    cur += SIMD_JOB_UNIT;
  }
}

void copy_bitmap_not_simd(BITS *to, BITS *from, int bitmap_len, int t_id) {
  int jobs = ((bitmap_len / SIMD_JOB_UNIT - 1) / THREAD_NUM + 1) * SIMD_JOB_UNIT;

  assert(jobs % SIMD_JOB_UNIT == 0);
  assert(bitmap_len % SIMD_JOB_UNIT == 0);
  assert(jobs * THREAD_NUM >= bitmap_len);

  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t_id * 2, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) < 0) {
    fprintf(stderr, "set thread affinity failed\n");
  }

  int cur = t_id * jobs;
  int end = (t_id + 1) * jobs;
  if (end > bitmap_len) end = bitmap_len;

  while (cur < end) {
    __m256i buf = _mm256_load_si256((__m256i *)(from + cur));
    buf = ~buf;
    _mm256_store_si256((__m256i *)(to + cur), buf);
    cur += SIMD_JOB_UNIT;
  }
}

void memset_numa0(BITS *p, int val, int n, int t_id) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t_id * 2, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) < 0) {
    fprintf(stderr, "set thread affinity failed\n");
  }
  int avg_workload = (n / (THREAD_NUM * SIMD_ALIGEN)) * SIMD_ALIGEN;
  int start = t_id * avg_workload;
  // int end = start + avg_workload;
  int end = t_id == (THREAD_NUM - 1) ? n : start + avg_workload;
  memset(p + start, val, (end - start) * sizeof(BITS));
  // if (t_id == THREAD_NUM) {
  //   BITS val_bits = 0U;
  //   for (int i = 0; i < sizeof(BITS); i++) {
  //     val_bits |= ((unsigned int)val) << (8 * i);
  //   }
  //   for (; end < n; end++) {
  //     p[end] = val_bits;
  //   }
  // }
}

void memset_mt(BITS *p, int val, int n) {
  std::thread threads[THREAD_NUM];
  for (int t_id = 0; t_id < THREAD_NUM; t_id++) {
    threads[t_id] = std::thread(memset_numa0, p, val, n, t_id);
  }
  for (int t_id = 0; t_id < THREAD_NUM; t_id++) {
    threads[t_id].join();
  }
}

void copy_filter_vector(BinDex *bindex, BITS *result, int k) {
  std::thread threads[THREAD_NUM];
  int bitmap_len = bits_num_needed(bindex->length);
  // BITS* result = (BITS*)aligned_alloc(SIMD_ALIGEN, bitmap_len *
  // sizeof(BITS));

  if (k < 0) {
    memset_mt(result, 0, bitmap_len);
    return;
  }

  if (k >= (K - 1)) {
    memset_mt(result, 0xFF, bitmap_len);  // Slower(?) than a loop
    return;
  }

  // simd copy
  // int mt_bitmap_n = (bitmap_len / SIMD_JOB_UNIT) * SIMD_JOB_UNIT; // must
  // be SIMD_JOB_UNIT aligened for (int i = 0; i < THREAD_NUM; i++)
  //     threads[i] = std::thread(copy_bitmap_simd, result,
  //     bindex->filterVectors[k], mt_bitmap_n, i);
  // for (int i = 0; i < THREAD_NUM; i++)
  //     threads[i].join();
  // memcpy(result + mt_bitmap_n, bindex->filterVectors[k] + mt_bitmap_n,
  // bitmap_len - mt_bitmap_n);

  // naive copy
  int avg_workload = bitmap_len / THREAD_NUM;
  int i;
  for (i = 0; i < THREAD_NUM - 1; i++) {
    threads[i] = std::thread(copy_bitmap, result + (i * avg_workload), bindex->filterVectors[k] + (i * avg_workload),
                             avg_workload, i);
  }
  threads[i] = std::thread(copy_bitmap, result + (i * avg_workload), bindex->filterVectors[k] + (i * avg_workload),
                           bitmap_len - (i * avg_workload), i);

  for (i = 0; i < THREAD_NUM; i++) {
    threads[i].join();
  }
}


void copy_filter_vector_in_GPU(BinDex *bindex, BITS *dev_bitmap, int k, bool negation) {
  std::thread threads[THREAD_NUM];
  int bitmap_len = bits_num_needed(bindex->length);

  if (k < 0) {
    cudaMemset(dev_bitmap, 0, bitmap_len * sizeof(BITS));
    return;
  }

  if (k >= (K - 1)) {
    cudaMemset(dev_bitmap, 0xFF, bitmap_len * sizeof(BITS));
    return;
  }

  if (!negation)
    GPUbitCopyWithCuda(dev_bitmap, bindex->filterVectorsInGPU[k], bitmap_len);
  else
    GPUbitCopyNegationWithCuda(dev_bitmap, bindex->filterVectorsInGPU[k], bitmap_len);
}

void copy_filter_vector_not(BinDex *bindex, BITS *result, int k) {
  std::thread threads[THREAD_NUM];
  int bitmap_len = bits_num_needed(bindex->length);
  // BITS* result = (BITS*)aligned_alloc(SIMD_ALIGEN, bitmap_len *
  // sizeof(BITS));

  if (k < 0) {
    memset_mt(result, 0xFF, bitmap_len);
    return;
  }

  if (k >= (K - 1)) {
    memset_mt(result, 0, bitmap_len);  // Slower(?) than a loop
    return;
  }

  // simd copy not
  int mt_bitmap_n = (bitmap_len / SIMD_JOB_UNIT) * SIMD_JOB_UNIT;  // must be SIMD_JOB_UNIT aligened
  for (int i = 0; i < THREAD_NUM; i++)
    threads[i] = std::thread(copy_bitmap_not_simd, result, bindex->filterVectors[k], mt_bitmap_n, i);
  for (int i = 0; i < THREAD_NUM; i++) threads[i].join();
  for (int i = 0; i < bitmap_len - mt_bitmap_n; i++) {
    (result + mt_bitmap_n)[i] = ~((bindex->filterVectors[k] + mt_bitmap_n)[i]);
  }

  // naive copy
  // for (int i = 0; i < THREAD_NUM; i++) {
  //   threads[i] = std::thread(copy_bitmap_not, result,
  //   bindex->filterVectors[k],
  //                            0, bitmap_len, i);
  // }

  // for (int i = 0; i < THREAD_NUM; i++) {
  //   threads[i].join();
  // }

  return;
}

void copy_filter_vector_bt(BinDex *bindex, BITS *result, int kl, int kr) {
  std::thread threads[THREAD_NUM];
  int bitmap_len = bits_num_needed(bindex->length);

  if (kr < 0) {
    // assert(0);
    // printf("1\n");
    memset_mt(result, 0, bitmap_len);
    return;
  } else if (kr >= (K - 1)) {
    // assert(0);
    // printf("2\n");
    copy_filter_vector_not(bindex, result, kl);
    return;
  }
  if (kl < 0) {
    // assert(0);
    // printf("3\n");
    copy_filter_vector(bindex, result, kr);
    return;
  } else if (kl >= (K - 1)) {
    // assert(0);
    // printf("4\n");
    memset_mt(result, 0, bitmap_len);  // Slower(?) than a loop
    return;
  }

  // simd copy_bt
  int mt_bitmap_n = (bitmap_len / SIMD_JOB_UNIT) * SIMD_JOB_UNIT;  // must be SIMD_JOB_UNIT aligened
  for (int i = 0; i < THREAD_NUM; i++)
    threads[i] =
        std::thread(copy_bitmap_bt_simd, result, bindex->filterVectors[kl], bindex->filterVectors[kr], mt_bitmap_n, i);
  for (int i = 0; i < THREAD_NUM; i++) threads[i].join();
  for (int i = 0; i < bitmap_len - mt_bitmap_n; i++) {
    (result + mt_bitmap_n)[i] =
        (~((bindex->filterVectors[kl] + mt_bitmap_n)[i])) & ((bindex->filterVectors[kr] + mt_bitmap_n)[i]);
  }

  // naive copy
  // for (int i = 0; i < THREAD_NUM; i++) {
  //   threads[i] = std::thread(copy_bitmap_bt, result,
  //   bindex->filterVectors[kl],
  //                            bindex->filterVectors[kr], 0, bitmap_len, i);
  // }

  // for (int i = 0; i < THREAD_NUM; i++) {
  //   threads[i].join();
  // }
}

void copy_filter_vector_bt_in_GPU(BinDex *bindex, BITS *result, int kl, int kr) {
  int bitmap_len = bits_num_needed(bindex->length);

  if (kr < 0) {
    // assert(0);
    // printf("1\n");
    // memset_mt(result, 0, bitmap_len);
    cudaMemset(result, 0, bitmap_len * sizeof(BITS));
    return;
  } else if (kr >= (K - 1)) {
    // assert(0);
    // printf("2\n");
    // copy_filter_vector_not(bindex, result, kl);
    copy_filter_vector_in_GPU(bindex, result, kl, true);
    return;
  }
  if (kl < 0) {
    // assert(0);
    // printf("3\n");
    // copy_filter_vector(bindex, result, kr);
    copy_filter_vector_in_GPU(bindex, result, kr);
    return;
  } else if (kl >= (K - 1)) {
    // assert(0);
    // printf("4\n");
    // memset_mt(result, 0, bitmap_len);  // Slower(?) than a loop
    cudaMemset(result, 0, bitmap_len * sizeof(BITS));
    return;
  }

  GPUbitCopySIMDWithCuda(result, 
                         bindex->filterVectorsInGPU[kl], 
                         bindex->filterVectorsInGPU[kr],
                         bitmap_len);

  // simd copy_bt
  // int mt_bitmap_n = (bitmap_len / SIMD_JOB_UNIT) * SIMD_JOB_UNIT;  // must be SIMD_JOB_UNIT aligened
  // for (int i = 0; i < THREAD_NUM; i++)
  //   threads[i] =
  //       std::thread(copy_bitmap_bt_simd, result, bindex->filterVectors[kl], bindex->filterVectors[kr], mt_bitmap_n, i);
  // for (int i = 0; i < THREAD_NUM; i++) threads[i].join();
  // for (int i = 0; i < bitmap_len - mt_bitmap_n; i++) {
  //   (result + mt_bitmap_n)[i] =
  //       (~((bindex->filterVectors[kl] + mt_bitmap_n)[i])) & ((bindex->filterVectors[kr] + mt_bitmap_n)[i]);
  // }
}

void copy_bitmap_xor_simd(BITS *to, BITS *bitmap1, BITS *bitmap2,
                          int bitmap_len, int t_id) {
  int jobs =
      ((bitmap_len / SIMD_JOB_UNIT - 1) / THREAD_NUM + 1) * SIMD_JOB_UNIT;

  assert(jobs % SIMD_JOB_UNIT == 0);
  assert(bitmap_len % SIMD_JOB_UNIT == 0);
  assert(jobs * THREAD_NUM >= bitmap_len);

  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t_id * 2, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) < 0) {
    fprintf(stderr, "set thread affinity failed\n");
  }

  int cur = t_id * jobs;
  int end = (t_id + 1) * jobs;
  if (end > bitmap_len) end = bitmap_len;

  while (cur < end) {
    __m256i buf1 = _mm256_load_si256((__m256i *)(bitmap1 + cur));
    __m256i buf2 = _mm256_load_si256((__m256i *)(bitmap2 + cur));
    __m256i buf = _mm256_andnot_si256(buf1, buf2);
    _mm256_store_si256((__m256i *)(to + cur), buf);
    cur += SIMD_JOB_UNIT;
  }
}

void copy_filter_vector_xor(BinDex *bindex, BITS *result, int kl, int kr) {
  std::thread threads[THREAD_NUM];
  int bitmap_len = bits_num_needed(bindex->length);

  if (kr < 0) {
    assert(0);
    // printf("1\n");
    memset_mt(result, 0, bitmap_len);
    return;
  } else if (kr >= (K - 1)) {
    assert(0);
    // printf("2\n");
    copy_filter_vector_not(bindex, result, kl);
    return;
  }
  if (kl < 0) {
    // assert(0);
    // printf("3\n");
    copy_filter_vector(bindex, result, kr);
    return;
  } else if (kl >= (K - 1)) {
    assert(0);
    // printf("4\n");
    memset_mt(result, 0, bitmap_len);  // Slower(?) than a loop
    return;
  }

  // simd copy_xor
  int mt_bitmap_n = (bitmap_len / SIMD_JOB_UNIT) *
                    SIMD_JOB_UNIT;  // must be SIMD_JOB_UNIT aligened
  for (int i = 0; i < THREAD_NUM; i++)
    threads[i] =
        std::thread(copy_bitmap_xor_simd, result, bindex->filterVectors[kl],
                    bindex->filterVectors[kr], mt_bitmap_n, i);
  for (int i = 0; i < THREAD_NUM; i++) threads[i].join();
  for (int i = 0; i < bitmap_len - mt_bitmap_n; i++) {
    (result + mt_bitmap_n)[i] = ((bindex->filterVectors[kl] + mt_bitmap_n)[i]) ^
                                ((bindex->filterVectors[kr] + mt_bitmap_n)[i]);
  }

  // naive copy
  // for (int i = 0; i < THREAD_NUM; i++) {
  //   threads[i] = std::thread(copy_bitmap_bt, result,
  //   hydex->filterVectors[kl],
  //                            hydex->filterVectors[kr], 0, bitmap_len, i);
  // }

  // for (int i = 0; i < THREAD_NUM; i++) {
  //   threads[i].join();
  // }
}

int in_which_area(BinDex *bindex, CODE compare) {
  if (compare < area_start_value(bindex->areas[0])) return -1;
  for (int i = 0; i < K; i++) {
    CODE area_sv = area_start_value(bindex->areas[i]);
    if (area_sv == compare) return i;
    if (area_sv > compare) return i - 1;
  }
  return K - 1;
}

int in_which_block(Area *area, CODE compare) {
  assert(compare >= area_start_value(area));
  int res = area->blockNum - 1;
  for (int i = 0; i < area->blockNum; i++) {
    CODE area_sv = block_start_value(area->blocks[i]);
    if (area_sv == compare) {
      res = i;
      break;
    }
    if (area_sv > compare) {
      res = i - 1;
      break;
    }
  }
  if (res) {
    pos_block *pre_blk = area->blocks[res - 1];
    if (pre_blk->val[pre_blk->length - 1] == compare) {
      res--;
    }
  }
  return res;
}

int on_which_pos(pos_block *pb, CODE compare) {
  // Find the first value which is no less than 'compare', return pb->length if
  // all data in the block are less than compare
  assert(compare >= pb->val[0]);
  int low = 0, high = pb->length, mid = (low + high) / 2;
  while (low < high) {
    if (pb->val[mid] >= compare) {
      high = mid;
    } else {
      low = mid + 1;
    }
    mid = (low + high) / 2;
  }
  return mid;
}

inline void refine(BITS *bitmap, POSTYPE pos) { bitmap[pos >> BITSSHIFT] ^= (1U << (BITSWIDTH - 1 - pos % BITSWIDTH)); }

void refine_positions(BITS *bitmap, POSTYPE *pos, POSTYPE n) {
  for (int i = 0; i < n; i++) {
    refine(bitmap, *(pos + i));
  }
}

void refine_positions_mt(BITS *bitmap, Area *area, int start_blk_idx, int end_blk_idx, int t_id) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t_id * 2, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) < 0) {
    fprintf(stderr, "set thread affinity failed\n");
  }
  int jobs = ROUNDUP_DIVIDE(end_blk_idx - start_blk_idx, THREAD_NUM);
  int cur = start_blk_idx + t_id * jobs;
  int end = start_blk_idx + (t_id + 1) * jobs;
  if (end > end_blk_idx) end = end_blk_idx;

  // int prefetch_stride = 6;
  while (cur < end) {
    POSTYPE *pos_list = area->blocks[cur]->pos;
    POSTYPE n = area->blocks[cur]->length;
    int i;
    for (i = 0; i + prefetch_stride < n; i++) {
      if(prefetch_stride) {
        __builtin_prefetch(&bitmap[*(pos_list + i + prefetch_stride) >> BITSSHIFT], 1, 1);
      }
      POSTYPE pos = *(pos_list + i);
      __sync_fetch_and_xor(&bitmap[pos >> BITSSHIFT], (1U << (BITSWIDTH - 1 - pos % BITSWIDTH)));
    }
    while (i < n) {
      POSTYPE pos = *(pos_list + i);
      __sync_fetch_and_xor(&bitmap[pos >> BITSSHIFT], (1U << (BITSWIDTH - 1 - pos % BITSWIDTH)));
      i++;
    }
    cur++;
  }
}

void refine_result_bitmap(BITS *bitmap_a, BITS *bitmap_b, int start_idx, int end_idx, int t_id) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t_id * 2, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) < 0) {
    fprintf(stderr, "set thread affinity failed\n");
  }
  int i;
  for (i = start_idx; i < end_idx; i++) {
    __sync_fetch_and_and(&bitmap_a[i], bitmap_b[i]);
  }
}

void set_eq_bitmap_mt(BITS *bitmap, Area *area, CODE compare, int start_blk_idx, int end_blk_idx, int t_id) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t_id * 2, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) < 0) {
    fprintf(stderr, "set thread affinity failed\n");
  }

  int jobs = ROUNDUP_DIVIDE(end_blk_idx - start_blk_idx, THREAD_NUM);
  int start = start_blk_idx + t_id * jobs;
  int end = start_blk_idx + (t_id + 1) * jobs;
  if (end > end_blk_idx) end = end_blk_idx;

  for (int i = start; i < end && block_start_value(area->blocks[i]) <= compare; i++) {
    pos_block *blk = area->blocks[i];
    POSTYPE *pos_list = blk->pos;
    POSTYPE n = blk->length;
    for (int j = 0; j < n && blk->val[j] <= compare; j++) {
      if (blk->val[j] == compare) {
        POSTYPE pos = *(pos_list + j);
        __sync_fetch_and_xor(&bitmap[pos >> BITSSHIFT], (1U << (BITSWIDTH - 1 - pos % BITSWIDTH)));
      }
    }
  }
}

void refine_positions_in_blks_mt(BITS *bitmap, Area *area, int start_blk_idx,
                                 int end_blk_idx, int t_id) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t_id * 2, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) < 0) {
    fprintf(stderr, "set thread affinity failed\n");
  }
  int jobs = ROUNDUP_DIVIDE(end_blk_idx - start_blk_idx, THREAD_NUM);
  int cur = start_blk_idx + t_id * jobs;
  int end = start_blk_idx + (t_id + 1) * jobs;
  if (end > end_blk_idx) end = end_blk_idx;

  int prefetch_stride = 6;
  while (cur < end) {
    POSTYPE *pos_list = area->blocks[cur]->pos;
    POSTYPE n = area->blocks[cur]->length;
    int i;
    for (i = 0; i + prefetch_stride < n; i++) {
      __builtin_prefetch(
          &bitmap[*(pos_list + i + prefetch_stride) >> BITSSHIFT], 1, 1);
      POSTYPE pos = *(pos_list + i);
      __sync_fetch_and_xor(&bitmap[pos >> BITSSHIFT],
                           (1U << (BITSWIDTH - 1 - pos % BITSWIDTH)));
    }
    while (i < n) {
      POSTYPE pos = *(pos_list + i);
      __sync_fetch_and_xor(&bitmap[pos >> BITSSHIFT],
                           (1U << (BITSWIDTH - 1 - pos % BITSWIDTH)));
      i++;
    }
    cur++;
  }
}

int find_appropriate_fv(BinDex *bindex, CODE compare) {
  if (compare < bindex->areaStartValues[0]) return -1;
  for (int i = 0; i < K; i++) {
    CODE area_sv = bindex->areaStartValues[i];
    if (area_sv == compare) return i;
    if (area_sv > compare) return i - 1;
  }
  // check if actually out of boundary here.
  // if so, return K
  // need extra check before use the return value avoid of error.
  if (compare <= bindex->data_max)
    return K - 1;
  else
    return K;
}

void bindex_scan_lt_in_GPU(BinDex *bindex, BITS *dev_bitmap, CODE compare, int bindex_id) {
  int bitmap_len = bits_num_needed(bindex->length);
  int area_idx = find_appropriate_fv(bindex, compare);


  // set common compare here for use in refine
  scan_max_compares[bindex_id][0] = bindex->data_min;
  scan_max_compares[bindex_id][1] = compare;

  // handle some boundary problems: <0, ==0, K-1, >K-1
  // <0: set all bits to 0. should not call rt. interrupt other procedures now. should cancel merge as well?
  // ==0: set all bits to 0. just scan one face: SC[this bindex] x MC[other bindexs].
  // K-1: area_idx = K - 1 may not cause problem now?
  // K: set all bits to 1. skip this face: SC[this bindex] x MC[other bindexs]

  if (area_idx < 0) {
    // 'compare' less than all raw_data, return all zero result
    // set skip to true so refine thread will be informed to skip
    scan_refine_mutex.lock();
    scan_skip_refine = true;
    scan_refine_mutex.unlock();
    return;
  }

  if (area_idx == 0) {
    // 'compare' less than all raw_data, return all zero result
    scan_refine_mutex.lock();
    if(!scan_skip_refine) {
      scan_skip_other_face[bindex_id] = true;
      scan_selected_compares[bindex_id][0] = bindex->data_min;
      scan_selected_compares[bindex_id][1] = compare;
      // printf("comapre[%d]: %u %u\n", bindex_id, scan_selected_compares[bindex_id][0], scan_selected_compares[bindex_id][1]);
      scan_refine_in_position += 1;
    }
    scan_refine_mutex.unlock();
    return;
  }

  if (area_idx > K - 1) {
    // set skip this bindex so rt will skip the scan for this face
    scan_refine_mutex.lock();
    if(!scan_skip_refine) {
      scan_skip_this_face[bindex_id] = true;
      scan_selected_compares[bindex_id][0] = bindex->data_min;
      scan_selected_compares[bindex_id][1] = compare;
      scan_refine_in_position += 1;
    }
    scan_refine_mutex.unlock();
    return;
  }

  // choose use inverse or normal
  bool inverse = false;
  if (area_idx < K - 1) {
    if (compare - bindex->areaStartValues[area_idx] > bindex->areaStartValues[area_idx + 1] - compare) {
      inverse = true;
      scan_inverse_this_face[bindex_id] = true;
    }
  }

  // set refine compares here
  scan_refine_mutex.lock();
  if (inverse) {
    scan_selected_compares[bindex_id][0] = compare;
    scan_selected_compares[bindex_id][1] = bindex->areaStartValues[area_idx + 1];
  } else {
    scan_selected_compares[bindex_id][0] = bindex->areaStartValues[area_idx];
    scan_selected_compares[bindex_id][1] = compare;
  }
  if(DEBUG_INFO) printf("comapre[%d]: %u %u\n", bindex_id, scan_selected_compares[bindex_id][0], scan_selected_compares[bindex_id][1]);
  scan_refine_in_position += 1;
  scan_refine_mutex.unlock();

  // we use the one small than compare here, so rt must return result to append (maybe with and)
  if(!scan_skip_refine) {
    if (inverse) {
      PRINT_EXCECUTION_TIME("copy",
                            copy_filter_vector_in_GPU(bindex, dev_bitmap, area_idx))
    }
    else {
      PRINT_EXCECUTION_TIME("copy",
                            copy_filter_vector_in_GPU(bindex, dev_bitmap, area_idx - 1))
    }
  }
}

void bindex_scan_gt_in_GPU(BinDex *bindex, BITS *dev_bitmap, CODE compare, int bindex_id) {
  compare = compare + 1;

  int bitmap_len = bits_num_needed(bindex->length);
  int area_idx = find_appropriate_fv(bindex, compare);

  // set common compare here for use in refine
  scan_max_compares[bindex_id][0] = compare;
  scan_max_compares[bindex_id][1] = bindex->data_max;

  // handle some boundary problems: <0, ==0, K-1, >K-1 (just like lt)
  // <0: set all bits to 1. skip this face: CS[this bindex] x CM[other bindexs]
  // ==0: may not cause problem here?
  // K-1: set all bits to 0. just scan one face: CS[this bindex] x CM[other bindexs].
  // K: set all bits to 0. should not call rt. interrupt other procedures now. should cancel merge as well?
  
  if (area_idx > K - 1) {

    // set skip to true so refine thread will be informed to skip
    scan_refine_mutex.lock();
    scan_skip_refine = true;
    scan_refine_mutex.unlock();

    // don't memset here to avoid duplicated memset
    // if(!scan_skip_refine) cudaMemset(dev_bitmap, 0xFF, bitmap_len);
    return;
  }

  if (area_idx == K - 1) {
    // 'compare' less than all raw_data, return all zero result
    scan_refine_mutex.lock();
    if(!scan_skip_refine) {
      scan_skip_other_face[bindex_id] = true;
      scan_selected_compares[bindex_id][0] = compare;
      scan_selected_compares[bindex_id][1] = bindex->data_max;
      // printf("comapre[%d]: %u %u\n", bindex_id, scan_selected_compares[bindex_id][0], scan_selected_compares[bindex_id][1]);
      scan_refine_in_position += 1;
    }
    scan_refine_mutex.unlock();
    // if(!scan_skip_refine) cudaMemset(dev_bitmap, 0, bitmap_len);
    return;
  }
  
  if (area_idx < 0) {
    // 'compare' less than all raw_data, return all 1 result
    // BITS* result = (BITS*)malloc(sizeof(BITS) * bitmap_len);

    scan_refine_mutex.lock();
    if(!scan_skip_refine) {
      scan_skip_this_face[bindex_id] = true;
      scan_selected_compares[bindex_id][0] = compare;
      scan_selected_compares[bindex_id][1] = bindex->data_max;
      scan_refine_in_position += 1;
    }
    scan_refine_mutex.unlock();

    // cudaMemset(dev_bitmap, 0xFF, bitmap_len);

    return;
  }


  // set refine compares here
  // scan_refine_mutex.lock();
  scan_refine_mutex.lock();
  scan_selected_compares[bindex_id][0] = compare;
  if (area_idx + 1 < K)
    scan_selected_compares[bindex_id][1] = bindex->areaStartValues[area_idx + 1];
  else 
    scan_selected_compares[bindex_id][1] = bindex->data_max;
  scan_refine_in_position += 1;
  scan_refine_mutex.unlock();

  // we use the one larger than compare here, so rt must return result to append (maybe with and)    retrun range -> | RT Scan | Bindex filter vector |
  PRINT_EXCECUTION_TIME("copy",
                        copy_filter_vector_in_GPU(bindex, dev_bitmap, area_idx, true))

  
}

void bindex_scan_bt_in_GPU(BinDex *bindex, BITS *result, CODE compare1, CODE compare2, int bindex_id) {
  assert(compare2 > compare1);
  compare1 = compare1 + 1;

  int bitmap_len = bits_num_needed(bindex->length);

  // x > compare1
  int area_idx_l = find_appropriate_fv(bindex, compare1);;
  if (area_idx_l < 0) {
    assert(0);
  }
  bool is_upper_fv_l = false;
  if (area_idx_l < K - 1) {
    if (compare1 - bindex->areaStartValues[area_idx_l] < bindex->areaStartValues[area_idx_l + 1] - compare1) {
      is_upper_fv_l = true;
      // scan_inverse_this_face[bindex_id] = true;
      scan_selected_compares[bindex_id][0] = bindex->areaStartValues[area_idx_l];
      scan_selected_compares[bindex_id][1] = compare1;
    } else {
      scan_selected_compares[bindex_id][0] = compare1;
      scan_selected_compares[bindex_id][1] = bindex->areaStartValues[area_idx_l + 1];
    }
  }

  // x < compare2
  int area_idx_r = find_appropriate_fv(bindex, compare2);
  bool is_upper_fv_r = false;
  if (area_idx_r < K - 1) {
    if (compare2 - bindex->areaStartValues[area_idx_r] < bindex->areaStartValues[area_idx_r + 1] - compare2) {
      is_upper_fv_r = true;
      // scan_inverse_this_face[bindex_id + 3] = true;
      scan_selected_compares[bindex_id + 3][0] = bindex->areaStartValues[area_idx_r];
      scan_selected_compares[bindex_id + 3][1] = compare2;
    } else {
      scan_selected_compares[bindex_id + 3][0] = compare2;
      scan_selected_compares[bindex_id + 3][1] = bindex->areaStartValues[area_idx_r + 1];
    }
  }


  PRINT_EXCECUTION_TIME("copy",
                        copy_filter_vector_bt_in_GPU(bindex,
                                              result,
                                              is_upper_fv_l ? (area_idx_l - 1) : (area_idx_l),
                                              is_upper_fv_r ? (area_idx_r - 1) : (area_idx_r))
                        )
}

void bindex_scan_eq_in_GPU(BinDex *bindex, BITS *result, CODE compare) {
  int bitmap_len = bits_num_needed(bindex->length);
  std::thread threads[THREAD_NUM];

  int area_idx = in_which_area(bindex, compare);
  assert(area_idx >= 0 && area_idx <= K - 1);

  if (area_idx != K - 1 &&
      area_start_value(bindex->areas[area_idx + 1]) == compare) {
    // nm > N / K
    // result1 = hydex_scan_lt(compare)
    // result2 = hydex_scan_lt(compare + 1)
    // result = result1 ^ result2
    std::thread threads[THREAD_NUM];
    int bitmap_len = bits_num_needed(bindex->length);

    // compare
    Area *area = bindex->areas[area_idx];
    int block_idx = in_which_block(area, compare);
    int pos_idx = on_which_pos(area->blocks[block_idx], compare);
    // Do an estimation to select the filter vector which is most
    // similar to the correct result
    int is_upper_fv;
    int start_blk_idx, end_blk_idx;
    if (block_idx < bindex->areas[area_idx]->blockNum / 2) {
      is_upper_fv = 1;
      start_blk_idx = 0;
      end_blk_idx = block_idx;
    } else {
      is_upper_fv = 0;
      start_blk_idx = block_idx + 1;
      end_blk_idx = area->blockNum;
    }

    // compare + 1
    CODE compare1 = compare + 1;
    int area_idx1 = in_which_area(bindex, compare1);
    if (area_idx1 < 0) {
      assert(0);
      return;
    }
    Area *area1 = bindex->areas[area_idx1];
    int block_idx1 = in_which_block(area1, compare1);
    int pos_idx1 = on_which_pos(area1->blocks[block_idx1], compare1);
    // Do an estimation to select the filter vector which is most
    // similar to the correct result
    int is_upper_fv1;
    int start_blk_idx1, end_blk_idx1;
    if (area_idx1 == 0 || (block_idx1 < bindex->areas[area_idx1]->blockNum / 2 &&
                           area_start_value(bindex->areas[area_idx1 - 1]) !=
                               area_start_value(area1))) {
      is_upper_fv1 = 1;
      start_blk_idx1 = 0;
      end_blk_idx1 = block_idx1;
    } else {
      is_upper_fv1 = 0;
      start_blk_idx1 = block_idx1 + 1;
      end_blk_idx1 = area1->blockNum;
    }

    // clang-format off
    PRINT_EXCECUTION_TIME("copy",
                        copy_filter_vector_xor(bindex,
                                              result,
                                              is_upper_fv ? (area_idx - 1) : (area_idx),
                                              is_upper_fv1 ? (area_idx1 - 1) : (area_idx1))
                        )

    PRINT_EXCECUTION_TIME("refine",
                        // refine left part
                        for (int i = 0; i < THREAD_NUM; i++)
                          threads[i] = std::thread(refine_positions_in_blks_mt, result, area, start_blk_idx, end_blk_idx, i);
                        for (int i = 0; i < THREAD_NUM; i++) threads[i].join();

                        if (is_upper_fv)
                          refine_positions(result, area->blocks[block_idx]->pos, pos_idx);
                        else
                          refine_positions(result, area->blocks[block_idx]->pos + pos_idx, area->blocks[block_idx]->length - pos_idx);

                        // refine right part
                        for (int i = 0; i < THREAD_NUM; i++)
                          threads[i] = std::thread(refine_positions_in_blks_mt, result, area1, start_blk_idx1, end_blk_idx1, i);
                        for (int i = 0; i < THREAD_NUM; i++) threads[i].join();

                        if (is_upper_fv1)
                          refine_positions(result, area1->blocks[block_idx1]->pos, pos_idx1);
                        else
                          refine_positions(result, area1->blocks[block_idx1]->pos + pos_idx1, area1->blocks[block_idx1]->length - pos_idx1);
                        )
    // clang-format on
  } else {
    // nm < N / K
    memset_mt(result, 0, bitmap_len);

    Area *area = bindex->areas[area_idx];
    int block_idx = in_which_block(area, compare);

    for (int i = 0; i < THREAD_NUM; i++) {
      threads[i] = std::thread(set_eq_bitmap_mt, result, area, compare,
                               block_idx, area->blockNum, i);
    }

    for (int i = 0; i < THREAD_NUM; i++) {
      threads[i].join();
    }
  }
}

void free_pos_block(pos_block *pb) {
  free(pb->pos);
  free(pb->val);
  free(pb);
}

void free_area(Area *area) {
  for (int i = 0; i < area->blockNum; i++) {
    free_pos_block(area->blocks[i]);
  }
  free(area);
}

void free_bindex(BinDex *bindex) {
  for (int i = 0; i < K - 1; i++) {
    free(bindex->filterVectors[i]);
  }
  for (int i = 0; i < K; i++) {
    free_area(bindex->areas[i]);
  }
  free(bindex);
}

std::vector<CODE> get_target_numbers(const char *s) {
  std::string input(s);
  std::stringstream ss(input);
  std::string value;
  std::vector<CODE> result;
  while (std::getline(ss, value, ',')) {
    result.push_back((CODE)stod(value));
  }
  return result;
}

std::vector<CODE> get_target_numbers(string s) {
  std::stringstream ss(s);
  std::string value;
  std::vector<CODE> result;
  while (std::getline(ss, value, ',')) {
    result.push_back((CODE)stod(value));
  }
  return result;
}

void getDataFromFile(char *DATA_PATH, CODE **initial_data, int bindex_num) {
  FILE *fp;
  if (!(fp = fopen(DATA_PATH, "rb"))) {
    printf("init_data_from_file: fopen(%s) faild\n", DATA_PATH);
    exit(-1);
  }
  printf("initing data from %s\n", DATA_PATH);

  for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
    initial_data[bindex_id] = (CODE *)malloc(N * sizeof(CODE));
    CODE *data = initial_data[bindex_id];
    if (fread(data, sizeof(uint32_t), N, fp) == 0) {
      printf("init_data_from_file: fread faild.\n");
      exit(-1);
    }
    printf("[CHECK] col %d  first num: %u  last num: %u\n", bindex_id, initial_data[bindex_id][0], initial_data[bindex_id][N - 1]);
  }
}

void compare_bitmap(BITS *bitmap_a, BITS *bitmap_b, int len, CODE **raw_data, int bindex_num) {
  int total_hit = 0;
  int true_hit = 0;
  for (int i = 0; i < len; i++) {
    int data_a = (bitmap_a[i >> BITSSHIFT] & (1U << (BITSWIDTH - 1 - i % BITSWIDTH)));
    int data_b = (bitmap_b[i >> BITSSHIFT] & (1U << (BITSWIDTH - 1 - i % BITSWIDTH)));
    if (data_a) {
      total_hit += 1;
      if (data_b) true_hit += 1;
    }
    if (data_a != data_b) {
      printf("[ERROR] check error in raw_data[%d]=", i);
      printf(" %u / %u / %u \n", raw_data[0][i], raw_data[1][i], raw_data[2][i]);
      printf("the correct is %#x, but we have %#x\n", data_a, data_b);
      for (int j = 0; j < bindex_num; j++) {
        printf("SC[%d] = [%u,%u], MC[%d] = [%u,%u]\n",j,scan_selected_compares[j][0],scan_selected_compares[j][1],
        j,scan_max_compares[j][0],scan_max_compares[j][1]);
      }
      break;
    }
  }
  printf("[CHECK]hit %d/%d\n", true_hit, total_hit);
  return;
}

void raw_scan(BinDex *bindex, BITS *bitmap, CODE target1, CODE target2, OPERATOR OP, CODE *raw_data, BITS* compare_bitmap) {
  for(int i = 0; i < bindex->length; i++) {
    bool hit = false;
    switch (OP)
    {
    case LT:
      if (raw_data[i] < target1) hit = true;
      break;
    case LE:
      if (raw_data[i] <= target1) hit = true;
      break;
    case GT:
      if (raw_data[i] > target1) hit = true;
      break;
    case GE:
      if (raw_data[i] >= target1) hit = true;
      break;
    case EQ:
      if (raw_data[i] == target1) hit = true;
      break;
    case BT:
      if (raw_data[i] > target1 && raw_data[i] < target2) hit = true;
      break;
    default:
      break;
    }
    if (hit) {
      // bitmap[i >> BITSSHIFT] |= (1U << (BITSWIDTH - 1 - i % BITSWIDTH));
      if (compare_bitmap != NULL) {
        int compare_bit = (compare_bitmap[i >> BITSSHIFT] & (1U << (BITSWIDTH - 1 - i % BITSWIDTH)));
        if(compare_bitmap == 0) {
          printf("[ERROR] check error in raw_data[%d]=", i);
          printf(" %u\n", raw_data[i]);
          break;
        }
      } else {
        refine(bitmap, i);
      }
    }
  }
}

void raw_scan_entry(std::vector<CODE>* target_l, std::vector<CODE>* target_r, std::string search_cmd, BinDex* bindex, BITS* bitmap, BITS* mergeBitmap, CODE* raw_data) {
  CODE target1, target2;
  
  for (int pi = 0; pi < target_l->size(); pi++) {
    target1 = (*target_l)[pi];
    if (target_r->size() != 0) {
      assert(search_cmd == "bt");
      target2 = (*target_r)[pi];
    }
    if (search_cmd == "bt" && target1 > target2) {
      std::swap(target1, target2);
    }

    if (search_cmd == "lt") {
      raw_scan(bindex, bitmap, target1, 0, LT, raw_data);
    } else if (search_cmd == "le") {
      raw_scan(bindex, bitmap, target1, 0, LE, raw_data);
    } else if (search_cmd == "gt") {
      raw_scan(bindex, bitmap, target1, 0, GT, raw_data);
    } else if (search_cmd == "ge") {
      raw_scan(bindex, bitmap, target1, 0, GE, raw_data);
    } else if (search_cmd == "eq") {
      raw_scan(bindex, bitmap, target1, 0, EQ, raw_data);
    } else if (search_cmd == "bt") {
      raw_scan(bindex, bitmap, target1, target2, BT, raw_data);
    }
  }

  /* for (int bindex_id = 1; bindex_id < bindex_num; bindex_id++) {
    for (int m = 0; m < bitmap_len; m++) {
      bitmap[0][m] = bitmap[0][m] & bitmap[m];
    }
  } */

  // CPU merge
  // int stride = 156250;
  // if (N / stride / CODEWIDTH > THREAD_NUM) {
  //   stride = N / THREAD_NUM / CODEWIDTH;
  //   printf("No enough threads, set stride to %d\n", stride);
  // }
  
  int max_idx = (N + CODEWIDTH - 1) / CODEWIDTH;
  int stride = (max_idx + THREAD_NUM - 1) / THREAD_NUM;

  if (mergeBitmap != bitmap) {
    std::thread threads[THREAD_NUM];
    int start_idx = 0;
    int end_idx = 0;
    size_t t_id = 0;
    while (end_idx < max_idx && t_id < THREAD_NUM) {
      end_idx = start_idx + stride;
      if (end_idx > max_idx) {
        end_idx = max_idx;
      }
      // printf("start idx: %d end idx: %d\n",start_idx, end_idx);
      // refine_result_bitmap(mergeBitmap, bitmap, start_idx, end_idx, t_id);
      threads[t_id] = std::thread(refine_result_bitmap, mergeBitmap, bitmap, start_idx, end_idx, t_id);
      start_idx += stride;
      t_id += 1;
    }
    for (int i = 0; i < THREAD_NUM; i++)
      threads[i].join();
  }
}

void scan_multithread_withGPU(std::vector<CODE>* target_l, std::vector<CODE>* target_r, std::string search_cmd, BinDex* bindex, BITS* bitmap, int bindex_id) {
  CODE target1, target2;
  
  for (int pi = 0; pi < target_l->size(); pi++) {
    printf("RUNNING %d\n", pi);
    target1 = (*target_l)[pi];
    if (target_r->size() != 0) {
      assert(search_cmd == "bt");
      target2 = (*target_r)[pi];
    }
    if (search_cmd == "bt" && target1 > target2) {
      std::swap(target1, target2);
    }

    if (search_cmd == "lt") {
      PRINT_EXCECUTION_TIME("lt", bindex_scan_lt_in_GPU(bindex, bitmap, target1, bindex_id));
    } else if (search_cmd == "le") {
      PRINT_EXCECUTION_TIME("le", bindex_scan_lt_in_GPU(bindex, bitmap, target1 + 1, bindex_id));
    } else if (search_cmd == "gt") {
      PRINT_EXCECUTION_TIME("gt", bindex_scan_gt_in_GPU(bindex, bitmap, target1, bindex_id));
    } else if (search_cmd == "ge") {
      PRINT_EXCECUTION_TIME("ge", bindex_scan_gt_in_GPU(bindex, bitmap, target1 - 1, bindex_id));
    } else if (search_cmd == "bt") {
      PRINT_EXCECUTION_TIME("bt", bindex_scan_bt_in_GPU(bindex, bitmap, target1, target2, bindex_id));
    }

    printf("\n");
  }
}

void special_eq_scan(CODE *target_l, CODE *target_r, BinDex **bindexs, BITS *dev_bitmap, const int bindex_num, string *search_cmd) {
  if(DEBUG_TIME_COUNT) timer.commonGetStartTime(13);
  if (DEBUG_INFO) {
    printf("[INFO] use special eq scan\n");
  }
  CODE **compares = (CODE **)malloc(bindex_num * sizeof(CODE *));
  CODE *dev_predicate = (CODE *)malloc(bindex_num * 2 * sizeof(CODE));
  CODE *range = (CODE *) malloc(sizeof(CODE) * 6);
  for (int i = 0; i < bindex_num; i++) {
    compares[i] = &(dev_predicate[i * 2]);
    range[i * 2] = bindexs[i]->data_min;
    range[i * 2 + 1] = bindexs[i]->data_max;
  }

  // prepare MC and SC
  int selected_id = 0;
  for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
    if (search_cmd[bindex_id] == "eq") {
      if (target_l[bindex_id] == 0 || target_l[bindex_id] == UINT32_MAX) {
        printf("[ERROR] query out of boundry!\n");
        exit(-1);
      }
      compares[bindex_id][0] = CODE(target_l[bindex_id] - 1);
      compares[bindex_id][1] = target_l[bindex_id] + 1;
      selected_id = bindex_id;
    }
    else if (search_cmd[bindex_id] == "lt") {
      compares[bindex_id][0] = bindexs[bindex_id]->data_min - 1;
      compares[bindex_id][1] = target_l[bindex_id];
      if (compares[bindex_id][0] > compares[bindex_id][1]) {
        swap(compares[bindex_id][0],compares[bindex_id][1]);
      }
    }
    else if (search_cmd[bindex_id] == "gt") {
      compares[bindex_id][0] = target_l[bindex_id];
      compares[bindex_id][1] = bindexs[bindex_id]->data_max + 1;
    }
    else {
      printf("[ERROR] not support yet!\n");
      exit(-1);
    }
  }
  

  if(DEBUG_INFO) {
    for (int i = 0; i < 6; i++) {
        printf("%u ", dev_predicate[i]);
    }
    printf("\n");
    printf("[INFO] compares prepared.\n");
  }

  GPURefineEqAreaWithCuda(bindexs, dev_bitmap, dev_predicate, selected_id, bindex_num);
  if(DEBUG_TIME_COUNT) timer.commonGetEndTime(13);
}

void refine_with_Cuda(BinDex **bindexs, BITS *dev_bitmap, const int bindex_num) {
  cudaError_t cudaStatus;

  CODE **compares = (CODE **)malloc(bindex_num * sizeof(CODE *));
  CODE *dev_predicate = (CODE *)malloc(bindex_num * 2 * sizeof(CODE));
  for (int i = 0; i < bindex_num; i++) {
    compares[i] = &(dev_predicate[i * 2]);
  }
  
  if(DEBUG_TIME_COUNT) timer.commonGetStartTime(13);
  // if there is a compare totally out of boundary, refine procedure can be skipped
  if (scan_skip_refine) {
    if(DEBUG_INFO) printf("[INFO] Search out of boundary, skip all refine.\n");
    if(DEBUG_TIME_COUNT) timer.commonGetEndTime(13);
    return;
  }

  // check if we can scan only one face
  for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
    if(scan_skip_other_face[bindex_id]) {
      if(DEBUG_INFO) printf("[INFO] %d face scan, other skipped.\n",bindex_id);
      // no matter use sc or mc
      compares[bindex_id][0] = scan_selected_compares[bindex_id][0];
      compares[bindex_id][1] = scan_selected_compares[bindex_id][1];

      for (int other_bindex_id = 0; other_bindex_id < bindex_num; other_bindex_id++) {
        if (other_bindex_id == bindex_id) continue;
        compares[other_bindex_id][0] = scan_max_compares[other_bindex_id][0];
        compares[other_bindex_id][1] = scan_max_compares[other_bindex_id][1];
      }

      for (int i = 0; i < bindex_num; i++) {
        if (compares[i][0] == compares[i][1]) {
          if(DEBUG_INFO) printf("[INFO] %d face scan skipped for the same compares[0] and compares[1].\n",bindex_id);
          if(DEBUG_TIME_COUNT) timer.commonGetEndTime(13);
          return;
        }
      }

      // Solve bound problem
      for (int i = 0; i < bindex_num; i++) {
        compares[i][0] -= 1;
      }

      // add refine here
      // send compares, dev_bitmap, the result is in dev_bitmap

      cudaStatus = GPURefineAreaWithCuda(bindexs, dev_bitmap, dev_predicate, bindex_id, bindex_num);
      if (cudaStatus != cudaSuccess) {
        fprintf(stderr, "refine failed, error type: %d\n", cudaStatus);
      }
      if(DEBUG_TIME_COUNT) timer.commonGetEndTime(13);
      return;
    }
  }

  double selectivity = 0.0;
  // rt scan every face
  /// split inversed face and non-inversed face first
  std::vector<int> inversed_face;
  std::vector<int> normal_face;
  for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
    if (scan_inverse_this_face[bindex_id]) {
      inversed_face.push_back(bindex_id);
    }
    else {
      normal_face.push_back(bindex_id);
    }
  }
  /// start refine
  int max_MS_face_count = 0;
  for (int i = 0; i < bindex_num; i++) {
    double face_selectivity = 1.0;
    bool inverse = false;
    int bindex_id;
    if (normal_face.size() != 0) {
      bindex_id = normal_face[0];
      normal_face.erase(normal_face.begin());
    }
    else if (inversed_face.size() != 0) {
      bindex_id = inversed_face[0];
      inversed_face.erase(inversed_face.begin());
      inverse = true;
    }

    int current_MS_face_count = 0;
    if(scan_skip_this_face[bindex_id]) {
      if(DEBUG_INFO) printf("[INFO] %d face scan skipped.\n",bindex_id);
      continue;
    }
    // select SC face
    compares[bindex_id][0] = scan_selected_compares[bindex_id][0];
    compares[bindex_id][1] = scan_selected_compares[bindex_id][1];

    // revise S and C here to avoid a < x < b scan
    // compares[bindex_id][0] -= 1.0;

    for (int other_bindex_id = 0; other_bindex_id < bindex_num; other_bindex_id++) {
      if (other_bindex_id == bindex_id) continue;
      if (current_MS_face_count < max_MS_face_count) {
        CODE S;
        if (inverse) 
          S = scan_selected_compares[other_bindex_id][1];
        else
          S = scan_selected_compares[other_bindex_id][0];
        if (scan_max_compares[other_bindex_id][0] < S) {
          compares[other_bindex_id][0] = scan_max_compares[other_bindex_id][0];
          compares[other_bindex_id][1] = S;
        }
        else {
          compares[other_bindex_id][0] = S;
          compares[other_bindex_id][1] = scan_max_compares[other_bindex_id][0];
        }
        current_MS_face_count += 1;
      } else {
        compares[other_bindex_id][0] = scan_max_compares[other_bindex_id][0];
        compares[other_bindex_id][1] = scan_max_compares[other_bindex_id][1];
      }
    }

    bool mid_skip_flag = false;
    for (int j = 0; j < bindex_num; j++) {
      if (compares[j][0] == compares[j][1]) {
        if(DEBUG_INFO) printf("[INFO] %d face scan skipped for the same compares[0] and compares[1].\n",bindex_id);
        mid_skip_flag = true;
        break;
      }
    }
    if (mid_skip_flag) continue;
    
    for (int i = 0; i < bindex_num; i++) {
      compares[i][0] -= 1;
    }

    if(DEBUG_INFO) {
      for (int i = 0; i < bindex_num; i++) {
        face_selectivity *= double(compares[i][1] - compares[i][0]) / double(bindexs[i]->data_max - bindexs[i]->data_min);
      }
      printf("face selectivity: %f\n", face_selectivity);
      selectivity += face_selectivity;
    }

    // add refine here
    // send compares, dev_bitmap, the result is in dev_bitmap
    cudaStatus = GPURefineAreaWithCuda(bindexs, dev_bitmap, dev_predicate, bindex_id, bindex_num, inverse);
    if (cudaStatus != cudaSuccess) {
      fprintf(stderr, "refine failed, error type: %d\n", cudaStatus);
    }
    
    max_MS_face_count += 1;
  }
  if(DEBUG_INFO) printf("total selectivity: %f\n", selectivity);
  if(DEBUG_TIME_COUNT) timer.commonGetEndTime(13);
  return;
}

void merge_with_GPU(BITS *merge_bitmap, BITS **dev_bitmaps, const int bindex_num, const int bindex_len) {
  timer.commonGetStartTime(15);

  int bitmap_len = bits_num_needed(bindex_len);
  if (scan_skip_refine) {
    if(DEBUG_INFO) printf("[INFO] Search out of boundary, skip all merge.\n");
    cudaMemset(merge_bitmap, 0, bitmap_len * sizeof(BITS));
    timer.commonGetEndTime(15);
    return;
  }

  for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
    if(scan_skip_other_face[bindex_id]) {
      if(DEBUG_INFO) printf("[INFO] %d face merge, other skipped.\n",bindex_id);
      cudaMemset(merge_bitmap, 0, bitmap_len * sizeof(BITS));
      timer.commonGetEndTime(15);
      return;
    }
  }

  for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
    if (merge_bitmap == dev_bitmaps[bindex_id]) {
      if (scan_skip_this_face[bindex_id]) {
        if(DEBUG_INFO) printf("[INFO] merge face %d skipped, set it to 0xFF\n", bindex_id);
        cudaMemset(merge_bitmap, 0xFF, bitmap_len * sizeof(BITS));
      }
      break;
    }
  }

  // int skip_num = 0;
  for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
    if (!scan_skip_this_face[bindex_id]) {
      if (merge_bitmap != dev_bitmaps[bindex_id]) {
        GPUbitAndWithCuda(merge_bitmap, dev_bitmaps[bindex_id], bindex_len);
      }
    }
    else {
      if(DEBUG_INFO) printf("[INFO] %d face merge skipped.\n",bindex_id);
    }
  }

  timer.commonGetEndTime(15);
  return;
}

int main(int argc, char *argv[]) {
  printf("N = %d\n", N);
  
  char opt;
  int selectivity;
  char DATA_PATH[256] = "\0";
  int bindex_num = 1;

  while ((opt = getopt(argc, argv, "hf:n:p:b:")) != -1) {
    switch (opt) {
      case 'h':
        printf(
            "Usage: %s \n"
            "[-l <left target list>] [-r <right target list>]"
            "[-i test inserting]"
            "[-p <scan-cmd-file>]"
            "[-f <input-file>]\n",
            argv[0]);
        exit(0);
      case 'f':
        strcpy(DATA_PATH, optarg);
        break;
      case 'b':
        bindex_num = atoi(optarg);
        break;
      case 'p':
        strcpy(scan_cmd_file, optarg);
        break;
      default:
        printf("Error: unknown option %c\n", (char)opt);
        exit(-1);
    }
  }
  assert(blockNumMax);
  assert(bindex_num >= 1);

  // initial data
  CODE *initial_data[MAX_BINDEX_NUM];

  if (!strlen(DATA_PATH)) {
    printf("initing data by random\n");
    for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
      initial_data[bindex_id] = (CODE *)malloc(N * sizeof(CODE));
      CODE *data = initial_data[bindex_id];
      std::random_device rd;
      std::mt19937 mt(rd());
      std::uniform_int_distribution<CODE> dist(MINCODE, MAXCODE);
      CODE mask = ((uint64_t)1 << (sizeof(CODE) * 8)) - 1;
      for (int i = 0; i < N; i++) {
        data[i] = dist(mt) & mask;
        assert(data[i] <= mask);
      }
    }
  } else {
    getDataFromFile(DATA_PATH, initial_data, bindex_num);
  }

  size_t avail_init_gpu_mem, total_gpu_mem;
  size_t avail_curr_gpu_mem, used, base_data = 0;
  cudaMemGetInfo( &avail_init_gpu_mem, &total_gpu_mem );

  timer.commonGetStartTime(20); // index build time
  BinDex *bindexs[MAX_BINDEX_NUM];
  for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
    printf("Build the bindex structure %d...\n", bindex_id);
    CODE *data = initial_data[bindex_id];
    bindexs[bindex_id] = (BinDex *)malloc(sizeof(BinDex));
    if (DEBUG_TIME_COUNT) timer.commonGetStartTime(0);
    init_bindex_in_GPU(bindexs[bindex_id], data, N, base_data);
    if (DEBUG_TIME_COUNT) timer.commonGetEndTime(0);
    printf("\n");
  }
  timer.commonGetEndTime(20);
  std::cout << "[Time] Index Build Time: " << timer.time[20] << " ms" << std::endl;

  cudaMemGetInfo( &avail_curr_gpu_mem, &total_gpu_mem );
  used = avail_init_gpu_mem - avail_curr_gpu_mem - base_data;
  cout << "[Mem] Device memory used(MB): " << 1.0 * used / (1 << 20) << endl;

  // BinDex Scan
  printf("BinDex scan...\n");

  // init result in CPU memory
  BITS *bitmap[MAX_BINDEX_NUM];
  int bitmap_len;
  for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
    bitmap_len = bits_num_needed(bindexs[bindex_id]->length);
    bitmap[bindex_id] = (BITS *)aligned_alloc(SIMD_ALIGEN, bitmap_len * sizeof(BITS));
    memset_mt(bitmap[bindex_id], 0xFF, bitmap_len);
  }
  bitmap[bindex_num] = (BITS *)aligned_alloc(SIMD_ALIGEN, bitmap_len * sizeof(BITS));
  memset_mt(bitmap[bindex_num], 0xFF, bitmap_len);

  // init result in GPU memory
  BITS *dev_bitmap[MAX_BINDEX_NUM];
  cudaError_t cudaStatus;
  for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
    bitmap_len = bits_num_needed(bindexs[bindex_id]->length);
    cudaStatus = cudaMalloc((void**)&(dev_bitmap[bindex_id]), bitmap_len * sizeof(BITS));
    if (cudaStatus != cudaSuccess) {
      fprintf(stderr, "cudaMalloc failed when init dev bitmap!");
      exit(-1);
    }
    cudaMemset(dev_bitmap[bindex_id], 0xFF, bitmap_len * sizeof(BITS));
  }
  cudaStatus = cudaMalloc((void**)&(dev_bitmap[bindex_num]), bitmap_len * sizeof(BITS));
  if (cudaStatus != cudaSuccess) {
    fprintf(stderr, "cudaMalloc failed when init dev bitmap!");
    exit(-1);
  }
  cudaMemset(dev_bitmap[bindex_num], 0xFF, bitmap_len * sizeof(BITS));

  ifstream fin;
  if (strlen(scan_cmd_file) == 0) {
    fin.open("test/scan_cmd.txt");
  } else {
    fin.open(scan_cmd_file);
  }
  int toExit = 1;
  while(toExit != -1) {
    std::vector<CODE> target_l[MAX_BINDEX_NUM];
    std::vector<CODE> target_r[MAX_BINDEX_NUM]; 
    CODE target_l_new[MAX_BINDEX_NUM];
    CODE target_r_new[MAX_BINDEX_NUM]; 
    string search_cmd[MAX_BINDEX_NUM];

    for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
      cout << "input [operator] [target_l] [target_r] (" << bindex_id + 1 << "/" << bindex_num << ")" << endl;
      string input;

      getline(fin, input);
      cout << input << endl;
      std::vector<std::string> cmds = stringSplit(input, ' ');
      if (cmds[0] == "exit") {
        toExit = -1;
        break;
      }
      search_cmd[bindex_id] = cmds[0];
      if (cmds.size() > 1) {
        target_l[bindex_id] = get_target_numbers(cmds[1]);
        target_l_new[bindex_id] = target_l[bindex_id][0];
      }
      if (cmds.size() > 2) {
        target_r[bindex_id] = get_target_numbers(cmds[2]);
        target_r_new[bindex_id] = target_r[bindex_id][0];
      }
    }

    if (toExit == -1) {
      break;
    }

#if ONLY_REFINE == 1
    printf("ONLY_REFINE\n");
    CODE predicate[6];
    for (int i = 0; i < bindex_num; i++) {
      predicate[i * 2] = target_l_new[i];
      predicate[i * 2 + 1] = target_r_new[i];
    }
    cudaMemset(dev_bitmap[0], 0, bitmap_len * sizeof(BITS));
    timer.commonGetStartTime(13);
    cudaStatus = GPURefineAreaWithCuda(bindexs, dev_bitmap[0], predicate, 0); // bindex_id=0 for TPCH-Q6
    if (cudaStatus != cudaSuccess) {
      fprintf(stderr, "refine failed, error type: %d\n", cudaStatus);
    }
    timer.commonGetEndTime(13);
#else
    // clean up refine slot
    scan_refine_in_position = 0;
    for(int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
      scan_selected_compares[bindex_id][0] = 0;
      scan_selected_compares[bindex_id][1] = 0;
      scan_max_compares[bindex_id][0] = 0;
      scan_max_compares[bindex_id][1] = 0;
      scan_skip_other_face[bindex_id] = false;
      scan_skip_this_face[bindex_id] = false;
      scan_inverse_this_face[bindex_id] = false;
    }
    scan_skip_refine = false;

    timer.commonGetStartTime(11);

    // special scan for = (eq) operator
    bool eq_scan = false;
    for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
      if (search_cmd[bindex_id] == "eq") {
        cudaMemset(dev_bitmap[0], 0, bitmap_len * sizeof(BITS));
        special_eq_scan(target_l_new, target_r_new, bindexs, dev_bitmap[0], bindex_num, search_cmd);
        eq_scan = true;
        break;
      }
    }

    if (!eq_scan) {
      assert(THREAD_NUM >= bindex_num);
      std::thread threads[THREAD_NUM];
      for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
        threads[bindex_id] = std::thread(scan_multithread_withGPU, 
                                          &(target_l[bindex_id]), 
                                          &(target_r[bindex_id]), 
                                          search_cmd[bindex_id], 
                                          bindexs[bindex_id], 
                                          dev_bitmap[bindex_id],
                                          bindex_id
        );
      }
      for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) threads[bindex_id].join();

      // merge should be done before refine now since new refine relies on dev_bitmap[0]
      merge_with_GPU(dev_bitmap[0], dev_bitmap, bindex_num, bindexs[0]->length);
      
  #if DEBUG_INFO == 1
      for (int i = 0; i < 6; i++) {
        printf("%u < x < %u\n", scan_selected_compares[i][0], scan_selected_compares[i][1]);
      }
  #endif
  #if ONLY_DATA_SIEVING == 0
      std::thread refine_thread = std::thread(refine_with_Cuda, bindexs, dev_bitmap[0], bindex_num);
      refine_thread.join();
  #endif
    }
    timer.commonGetEndTime(11);
#endif

    // transfer GPU result back to memory
    BITS *h_result;
    cudaStatus = cudaMallocHost(&h_result, bits_num_needed(bindexs[0]->length) * sizeof(BITS));
    if (cudaStatus != cudaSuccess) {
        fprintf(stderr, "[ERROR]cudaMallocHost: %s\n", cudaGetErrorString(cudaStatus));
        exit(-1);
    }
    timer.commonGetStartTime(12);
    cudaStatus = cudaMemcpy(h_result, dev_bitmap[0], bits_num_needed(bindexs[0]->length) * sizeof(BITS), cudaMemcpyDeviceToHost); // only transfer bindex[0] here. may have some problems.
    if (cudaStatus != cudaSuccess) {
        fprintf(stderr, "[ERROR]Result transfer, cudaMemcpy failed!\n");
        fprintf(stderr, "[ERROR]Result transfer: %s\n", cudaGetErrorString(cudaStatus));
        exit(-1);
    }
    timer.commonGetEndTime(12);
    
    timer.showTime();
    timer.clear();

    // check jobs
    BITS *check_bitmap[MAX_BINDEX_NUM];
    int bitmap_len;
    for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
      bitmap_len = bits_num_needed(bindexs[bindex_id]->length);
      check_bitmap[bindex_id] = (BITS *)aligned_alloc(SIMD_ALIGEN, bitmap_len * sizeof(BITS));
      memset_mt(check_bitmap[bindex_id], 0x0, bitmap_len);
    }

    /// check final result 
    printf("[CHECK]check final result.\n");
    for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
      raw_scan_entry(
          &(target_l[bindex_id]),
          &(target_r[bindex_id]),
          search_cmd[bindex_id],
          bindexs[bindex_id],
          check_bitmap[bindex_id],
          check_bitmap[0],
          initial_data[bindex_id]);
    }

#if ONLY_DATA_SIEVING == 0
    compare_bitmap(check_bitmap[0], h_result, bindexs[0]->length, initial_data, bindex_num);
    printf("[CHECK]check final result done.\n\n");
#endif

    for (int i = 0; i < bitmap_len; i++) {
      if (h_result[i] != 0) {
        printf("[CHECK] not all 0!\n");
        break;
      }
    }
    cudaFreeHost(h_result);
    for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) free(check_bitmap[bindex_id]);
  }

  // clean jobs
  for (int bindex_id = 0; bindex_id < bindex_num; bindex_id++) {
    free(initial_data[bindex_id]);
    free_bindex(bindexs[bindex_id]);
    free(bitmap[bindex_id]);
  }
  return 0;
}
