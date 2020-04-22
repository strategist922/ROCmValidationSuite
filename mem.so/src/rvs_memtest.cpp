/*
 * Illinois Open Source License
 *
 * University of Illinois/NCSA
 * Open Source License
 *
 * Copyright � 2009,    University of Illinois.  All rights reserved.
 *
 * Developed by:
 *
 * Innovative Systems Lab
 * National Center for Supercomputing Applications
 * http://www.ncsa.uiuc.edu/AboutUs/Directorates/ISL.html
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal with
 * the Software without restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * * Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimers.
 *
 * * Redistributions in binary form must reproduce the above copyright notice, this list
 * of conditions and the following disclaimers in the documentation and/or other materials
 * provided with the distribution.
 *
 * * Neither the names of the Innovative Systems Lab, the National Center for Supercomputing
 * Applications, nor the names of its contributors may be used to endorse or promote products
 * derived from this Software without specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT
 * OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS WITH THE SOFTWARE.
 */


#include <iostream>
#include <pthread.h>
#include <thread>
#include <chrono>
#include <cstdio>
#include <sys/time.h>
#include <unistd.h>
#include<sstream>


#include "hip/hip_runtime.h"
#include "hip/hip_runtime_api.h"

#include "include/rvs_memkernel.h"
#include "include/rvs_memworker.h"
#include "include/rvs_memtest.h"
#include "include/rvsloglp.h"


#define DEBUG_PRINTF(fmt,...) do {					\
	    PRINTF(fmt, ##__VA_ARGS__);					\
}while(0)


#define PRINTF(fmt,...) do{						\
	printf("[%s][%s][%d]:" fmt, time_string(), hostname, gpu_idx, ##__VA_ARGS__); \
	fflush(stdout);							\
} while(0)

#define FPRINTF(fmt,...) do{						\
  fprintf(stderr, "[%s][%s][%d]:" fmt, time_string(), hostname, gpu_idx, ##__VA_ARGS__); \
	fflush(stderr);							\
} while(0)

#define RECORD_ERR(count_of_errors, start_addr, expect, current) do{		\
	unsigned int idx = atomicAdd(count_of_errors, 1);		\
	idx = idx % MAX_ERR_RECORD_COUNT;		\
	ptFailedAdress[idx] = (unsigned long)start_addr;		\
	expectedValue[idx] = (unsigned long)expect;	\
	ptCurrentValue[idx] = (unsigned long)current;	\
	ptValueOfStartAddr[idx] = (unsigned long)(*start_addr);	\
}while(0)



#define SHOW_PROGRESS(msg, i, tot_num_blocks)				\
    hipDeviceSynchronize();						\
    unsigned int num_checked_blocks =  i+GRIDSIZE <= tot_num_blocks? i+GRIDSIZE: tot_num_blocks; \
	  std::cerr << msg << ": " << num_checked_blocks << " out of " << tot_num_blocks <<" blocks finished\n";

#define HIP_ASSERT(x) (assert((x)==hipSuccess))

#define rvs_DEVICE_SERIAL_BUFFER_SIZE 0
#define MAX_ERR_RECORD_COUNT          10
#define MAX_NUM_GPUS                  128
#define MAX_ITERATION                 3
#define STRESS_BLOCKSIZE              64
#define STRESS_GRIDSIZE               (1024*32)
#define ERR_MSG_LENGTH                4096

void InitRvsMemtest()
{
    *ptCntOfError = 0;
    ptFailedAdress = 0;
    tot_num_blocks = 0;
    max_num_blocks = 0;
    exit_on_error = 0;
    num_iterations = 0;
    blocks = 512;
    threadsPerBlock = 256;
    global_pattern = 0;
    global_pattern_long = 0;
    num_passes = 1;
    verbose = 0;
    interactive = 0;

}

void atomic_inc(unsigned int* value)
{
    std::lock_guard<std::mutex> lck(mtx_mem_test);  
    (*value)= (*value) + 1;
}

unsigned int atomic_read(unsigned int* value)
{
    unsigned int ret;

    std::lock_guard<std::mutex> lck(mtx_mem_test);  
    ret = *value;

    return ret;
}

unsigned int error_checking(const char* pmsg, unsigned int blockidx)
{
    unsigned long host_err_addr[MAX_ERR_RECORD_COUNT];
    unsigned long host_err_expect[MAX_ERR_RECORD_COUNT];
    unsigned long host_err_current[MAX_ERR_RECORD_COUNT];
    unsigned long host_err_second_read[MAX_ERR_RECORD_COUNT];
    unsigned int  numOfErrors = 0;
    unsigned int  i;

    std::cout << "\n" <<  pmsg << " block id :" << blockidx << "\n";

    HIP_CHECK(hipMemcpy(&numOfErrors, ptCntOfError, sizeof(unsigned int), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(&host_err_addr[0], &ptFailedAdress, sizeof(unsigned long)*MAX_ERR_RECORD_COUNT, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(&host_err_expect[0], &expectedValue, sizeof(unsigned long)*MAX_ERR_RECORD_COUNT, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(&host_err_current[0], &ptCurrentValue, sizeof(unsigned long)*MAX_ERR_RECORD_COUNT, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(&host_err_second_read[0], &ptValueOfStartAddr, sizeof(unsigned long)*MAX_ERR_RECORD_COUNT, hipMemcpyDeviceToHost));

    std::string emsg;
    std::ostringstream msg;
    std::string driver_info;
    std::string devSerialNum;

    if (numOfErrors){
      std::cerr << "ERROR: the last : " <<   MIN(MAX_ERR_RECORD_COUNT, numOfErrors) << " : error addresses are: \n";

	      for (i = 0; i < MIN(MAX_ERR_RECORD_COUNT, numOfErrors); i++){
            std::cerr << (void*)host_err_addr[i] << "\n";
	      }

        std::cerr << "\n";

	      for (i =0; i < MIN(MAX_ERR_RECORD_COUNT, numOfErrors); i++){

          std::cerr << " ERROR:" << i << " th error, expected value=0x" << host_err_expect[i] << "current value=0x" << host_err_current[i] << "current value=0x" << host_err_current[i] <<  "diff=0x" <<  (host_err_expect[i] ^ host_err_current[i]) << " second_ read=0x " << host_err_second_read[i] << "expect=0x" << host_err_expect[i] << "diff with expected value=0x" <<  (host_err_expect[i] ^ host_err_second_read[i]) << "\n";

            //std::cerr << msg.str() << "\n";  
	    }


	    hipMemset((void *)&ptCntOfError, 0, sizeof(unsigned int));
	    hipMemset(&ptFailedAdress[0], 0, sizeof(unsigned long)*MAX_ERR_RECORD_COUNT);;
	    hipMemset((void*)&expectedValue[0], 0, sizeof(unsigned long)*MAX_ERR_RECORD_COUNT);;
	    hipMemset((void*)&ptCurrentValue[0], 0, sizeof(unsigned long)*MAX_ERR_RECORD_COUNT);;

	    if (exit_on_error){
	        hipDeviceReset();
	        exit(ERR_BAD_STATE);
	    }
    }

    return numOfErrors;
}

unsigned int get_random_num(void)
{
    struct timeval t0;

    if (gettimeofday(&t0, NULL) !=0){

	      fprintf(stderr, "ERROR: gettimeofday() failed\n");
	      exit(ERR_GENERAL);
    }

    unsigned int seed= (unsigned int)t0.tv_sec;
    srand(seed);

    return rand_r(&seed);
}

uint64_t get_random_num_long(void)
{
    struct timeval t0;

    if (gettimeofday(&t0, NULL) !=0){
	    fprintf(stderr, "ERROR: gettimeofday() failed\n");
	    exit(ERR_GENERAL);
    }

    unsigned int seed= (unsigned int)t0.tv_sec;
    srand(seed);

    unsigned int a = rand_r(&seed);
    unsigned int b = rand_r(&seed);

    uint64_t ret =  ((uint64_t)a) << 32;
    ret |= ((uint64_t)b);

    return ret;
}

__global__  void kernel_test0_global_write(char* _ptr, char* _end_ptr, unsigned long *ptDebugValue)
 {
     unsigned int* ptr = (unsigned int*)_ptr;
     unsigned int* end_ptr = (unsigned int*)_end_ptr;
     unsigned int* orig_ptr = ptr;
     unsigned int pattern = 1;
     unsigned long mask = 4;

     *ptr = pattern;

     while(ptr < end_ptr){
         ptr = (unsigned int*) ( ((unsigned long)orig_ptr) | mask);

         if (ptr == orig_ptr){
             mask = mask <<1;
             continue;
         }

         if (ptr >= end_ptr){
             break;
         }

         *ptr = pattern;

         pattern = pattern << 1;
         mask = mask << 1;
     }
     return;
 }

 __global__ void kernel_test0_write(char* _ptr, char* end_ptr)
{
    unsigned int* orig_ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);
    unsigned int* ptr = orig_ptr;

    if (ptr >= (unsigned int*) end_ptr) {
	      return;
    }

    unsigned int* block_end = orig_ptr + BLOCKSIZE/sizeof(unsigned int);
    unsigned int pattern = 1;
    unsigned long mask = 4;

    *ptr = pattern;

    while(ptr < block_end){
	    ptr = (unsigned int*) ( ((unsigned long)orig_ptr) | mask);

	    if (ptr == orig_ptr){
	        mask = mask <<1;
	        continue;
	    }

	    if (ptr >= block_end){
	        break;
	    }

	    *ptr = pattern;

	    pattern = pattern << 1;
	    mask = mask << 1;
    }

    return;
}

__global__ void kernel_test0_global_read(char* _ptr, char* _end_ptr, unsigned int* err_count, unsigned long* ptFailedAdress,
			 unsigned long* expectedValue, unsigned long* currentError, unsigned long* ptValueOfStartAddr)
{
    unsigned int* ptr = (unsigned int*)_ptr;
    unsigned int* end_ptr = (unsigned int*)_end_ptr;
    unsigned int* orig_ptr = ptr;
    unsigned int pattern = 1;
    unsigned long mask = 4;

    if (*ptr != pattern){
	      RECORD_ERR(err_count, ptr, pattern, *ptr);
    }

    while(ptr < end_ptr){
	      ptr = (unsigned int*) ( ((unsigned long)orig_ptr) | mask);

        *ptDebugValue = 4444;

	      if (ptr == orig_ptr){
	        mask = mask << 1;
	        continue;
	      }

	      if (ptr >= end_ptr){
	          break;
	      }

	      if (*ptr != pattern){
	          RECORD_ERR(err_count, ptr, pattern, *ptr);
	      }

	      pattern = pattern << 1;
	      mask = mask << 1;
    }

    return;
}

__global__ void kernel_test0_read(char* _ptr, char* end_ptr, unsigned int* err, unsigned long* ptFailedAdress,
		  unsigned long* expectedValue, unsigned long* currentError, unsigned long* ptValueOfStartAddr)
{
    unsigned int* orig_ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);;
    unsigned int* ptr = orig_ptr;

    if (ptr >= (unsigned int*) end_ptr) {
	    return;
    }

    unsigned int* block_end = orig_ptr + BLOCKSIZE/sizeof(unsigned int);

    unsigned int pattern = 1;

    unsigned long mask = 4;

    if (*ptr != pattern){
	    RECORD_ERR(err, ptr, pattern, *ptr);
    }

    while(ptr < block_end){

	    ptr = (unsigned int*) ( ((unsigned long)orig_ptr) | mask);
	    if (ptr == orig_ptr){
	        mask = mask <<1;
	        continue;
	    }

	    if (ptr >= block_end){
	      break;
	    }

	    if (*ptr != pattern){
	      RECORD_ERR(err, ptr, pattern, *ptr);
	    }

	    pattern = pattern << 1;
	    mask = mask << 1;
    }

    return;
}



/************************************************************************
 * Test0 [Walking 1 bit]
 * This test changes one bit a time in memory address to see it
 * goes to a different memory location. It is designed to test
 * the address wires.
 *
 **************************************************************************/

void test0(char* ptr, unsigned int tot_num_blocks)
{
    unsigned int    i;
    char* end_ptr = ptr + tot_num_blocks* BLOCKSIZE;
    unsigned int debugValue;

    error_checking("\nStarting test0: Change one bit memory addresss  ",  0);

    //test global address
    hipLaunchKernelGGL(kernel_test0_global_write,   /* compute kernel*/
                          dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                        ptr , end_ptr, ptDebugValue);


    hipLaunchKernelGGL(kernel_test0_global_read,   /* compute kernel*/
                        dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
                        ptr, end_ptr, ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr);

    error_checking("Test0 on global address",  0);

    for(unsigned int ite = 0; ite < num_iterations; ite++){
	    for (i = 0; i < tot_num_blocks; i += GRIDSIZE){
	        dim3 grid;

	        grid.x= GRIDSIZE;
          hipLaunchKernelGGL(kernel_test0_write,   /* compute kernel*/
                                dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                              ptr + i * BLOCKSIZE, end_ptr); 
	    }

	    for (i=0;i < tot_num_blocks; i+= GRIDSIZE){
	        dim3 grid;

	        grid.x= GRIDSIZE;

          hipLaunchKernelGGL(kernel_test0_read,
                        dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
                        ptr + i * BLOCKSIZE, end_ptr, ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr); 

	    }

    }

    error_checking("Test0 checking complete :: ",  i);
    return;

}

/*********************************************************************************
 * test1
 * Each Memory location is filled with its own address. The next kernel checks if the
 * value in each memory location still agrees with the address.
 *
 ********************************************************************************/
__global__ void 
kernel_test1_write(char* _ptr, char* end_ptr, unsigned int* err)
{
    unsigned int i;
    unsigned long* ptr = (unsigned long*) (_ptr + blockIdx.x*BLOCKSIZE);

    if (ptr >= (unsigned long*) end_ptr) {
	      return;
    }

    for (i = 0;i < BLOCKSIZE/sizeof(unsigned long); i++){
	      ptr[i] =(unsigned long) & ptr[i];
    }

    return;
}

__global__ void 
kernel_test1_read(char* _ptr, char* end_ptr, unsigned int* err, unsigned long* ptFailedAdress,
		  unsigned long* expectedValue, unsigned long* currentError, unsigned long* ptValueOfStartAddr)
{
    unsigned int i;
    unsigned long* ptr = (unsigned long*) (_ptr + blockIdx.x*BLOCKSIZE);

    if (ptr >= (unsigned long*) end_ptr) {
	      return;
    }


    for (i = 0;i < BLOCKSIZE/sizeof(unsigned long); i++){
	    if (ptr[i] != (unsigned long)& ptr[i]){
	        RECORD_ERR(err, &ptr[i], (unsigned long)&ptr[i], ptr[i]);
	    }
    }

    return;
}

void test1(char* ptr, unsigned int tot_num_blocks)
{
    unsigned int i;
    char* end_ptr = ptr + tot_num_blocks* BLOCKSIZE;

    for (i=0;i < tot_num_blocks; i+= GRIDSIZE){
	    dim3 grid;

	    grid.x= GRIDSIZE;
      hipLaunchKernelGGL(kernel_test1_write,
                            dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                          ptr + i*BLOCKSIZE, end_ptr, ptCntOfError); 
	    SHOW_PROGRESS("Test1 on writing", i, tot_num_blocks);
    }

    for (i=0;i < tot_num_blocks; i+= GRIDSIZE){
	    dim3 grid;

	    grid.x= GRIDSIZE;
      hipLaunchKernelGGL(kernel_test1_read,
                            dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                          ptr + i*BLOCKSIZE, end_ptr, ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr);
	    SHOW_PROGRESS("\nTest1 on reading", i, tot_num_blocks);
    }

    return;

}



/******************************************************************************
 * Test 2 [Moving inversions, ones&zeros]
 * This test uses the moving inversions algorithm with patterns of all
 * ones and zeros.
 *
 ****************************************************************************/

__global__ void 
kernel_move_inv_write(char* _ptr, char* end_ptr, unsigned int pattern)
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);

    if (ptr >= (unsigned int*) end_ptr) {
	    return;
    }

    for (i = 0;i < BLOCKSIZE/sizeof(unsigned int); i++){
	      ptr[i] = pattern;
    }

    return;
}


__global__ void 
kernel_move_inv_readwrite(char* _ptr, char* end_ptr, unsigned int p1, unsigned int p2, unsigned int* err,
			  unsigned long* ptFailedAdress, unsigned long* expectedValue, unsigned long* currentError, unsigned long* ptValueOfStartAddr)
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);

    if (ptr >= (unsigned int*) end_ptr) {
	      return;
    }

    for (i = 0;i < BLOCKSIZE/sizeof(unsigned int); i++){
	      if (ptr[i] != p1){
	         RECORD_ERR(err, &ptr[i], p1, ptr[i]);
	      }

	      ptr[i] = p2;
    }

    return;
}


__global__ void 
kernel_move_inv_read(char* _ptr, char* end_ptr,  unsigned int pattern, unsigned int* err,
		     unsigned long* ptFailedAdress, unsigned long* expectedValue, unsigned long* currentError, unsigned long* ptValueOfStartAddr )
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);

    if (ptr >= (unsigned int*) end_ptr) {
	      return;
    }

    for (i = 0;i < BLOCKSIZE/sizeof(unsigned int); i++){
	      if (ptr[i] != pattern){
	          RECORD_ERR(err, &ptr[i], pattern, ptr[i]);
	      }
    }

    return;
}


unsigned int  move_inv_test(char* ptr, unsigned int tot_num_blocks, unsigned int p1, unsigned p2)
{
    unsigned int i;
    unsigned int err = 0;
    char* end_ptr = ptr + tot_num_blocks* BLOCKSIZE;

    for (i= 0;i < tot_num_blocks; i+= GRIDSIZE){
	      dim3 grid;

	      grid.x= GRIDSIZE;
        hipLaunchKernelGGL(kernel_move_inv_write,
                         dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                       ptr + i*BLOCKSIZE, end_ptr, p1); 
	      error_checking("Move inv writing to blocks",  i);
        SHOW_PROGRESS("move_inv_write", i, tot_num_blocks);

    }


    for (i=0; i < tot_num_blocks; i+= GRIDSIZE){
	      dim3 grid;

	      grid.x= GRIDSIZE;
        hipLaunchKernelGGL(kernel_move_inv_readwrite,
                         dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                       ptr + i*BLOCKSIZE, end_ptr, p1, p2, ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr); 
	      err += error_checking("Move inv reading and writing to blocks",  i);
        SHOW_PROGRESS("move_inv_readwrite", i, tot_num_blocks);
    }

    for (i=0; i < tot_num_blocks; i+= GRIDSIZE){
	      dim3 grid;

	      grid.x= GRIDSIZE;
        hipLaunchKernelGGL(kernel_move_inv_read,
                         dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                       ptr + i*BLOCKSIZE, end_ptr, p2, ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr); 
	      err += error_checking("Move inv reading from blocks",  i);
        SHOW_PROGRESS("move_inv_read", i, tot_num_blocks);
    }

    return err;

}


void test2(char* ptr, unsigned int tot_num_blocks)
{
    unsigned int p1 = 0;
    unsigned int p2 = ~p1;

    std::cout << "\nTest2: Moving inversions test, with pattern " << p1 << " and " << p2 << "\n";
    move_inv_test(ptr, tot_num_blocks, p1, p2);

    std::cout << "\nTest2: Moving inversions test, with pattern " << p2 << " and " << p1 << "\n";
    move_inv_test(ptr, tot_num_blocks, p2, p1);

}


/*************************************************************************
 *
 * Test 3 [Moving inversions, 8 bit pat]
 * This is the same as test 1 but uses a 8 bit wide pattern of
 * "walking" ones and zeros.  This test will better detect subtle errors
 * in "wide" memory chips.
 *
 **************************************************************************/


void test3(char* ptr, unsigned int tot_num_blocks)
{
    unsigned int p0=0x80;
    unsigned int p1 = p0 | (p0 << 8) | (p0 << 16) | (p0 << 24);
    unsigned int p2 = ~p1;

    std::cout << "Test3: Moving inversions test, with pattern " << p1 << " and " << p2 << "\n";
    move_inv_test(ptr, tot_num_blocks, p1, p2);

    std::cout << "Test3: Moving inversions test, with pattern " <<  p2 << " and " << p1 << "\n";
    move_inv_test(ptr, tot_num_blocks, p2, p1);

}


/************************************************************************************
 * Test 4 [Moving inversions, random pattern]
 * Test 4 uses the same algorithm as test 1 but the data pattern is a
 * random number and it's complement. This test is particularly effective
 * in finding difficult to detect data sensitive errors. A total of 60
 * patterns are used. The random number sequence is different with each pass
 * so multiple passes increase effectiveness.
 *
 *************************************************************************************/

void test4(char* ptr, unsigned int tot_num_blocks)
{
    unsigned int p1;


    if (global_pattern == 0){
	    p1 = get_random_num();
    }else{
	    p1 = global_pattern;
    }

    unsigned int p2 = ~p1;
    unsigned int err = 0;
    unsigned int iteration = 0;

    std::cout << "Test4: Moving inversions test, with random pattern " << p1 << " and " <<  p2;

    repeat:
          err += move_inv_test(ptr, tot_num_blocks, p1, p2);

          if (err == 0 && iteration == 0){
	          return;
          }

          if (iteration < MAX_ITERATION){

            std::cout << iteration <<"th repeating test4 because there are " << err <<"errors found in last run \n";
	          iteration++;
	          err = 0;
	          goto repeat;
          }
}


/************************************************************************************
 * Test 5 [Block move, 64 moves]
 * This test stresses memory by moving block memories. Memory is initialized
 * with shifting patterns that are inverted every 8 bytes.  Then blocks
 * of memory are moved around.  After the moves
 * are completed the data patterns are checked.  Because the data is checked
 * only after the memory moves are completed it is not possible to know
 * where the error occurred.  The addresses reported are only for where the
 * bad pattern was found.
 *
 *
 *************************************************************************************/

__global__ void kernel_test5_init(char* _ptr, char* end_ptr)
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);

    if (ptr >= (unsigned int*) end_ptr) {
	      return;
    }

    unsigned int p1 = 1;

    for (i = 0;i < BLOCKSIZE/sizeof(unsigned int); i+=16){
	      unsigned int p2 = ~p1;

	      ptr[i] = p1;
	      ptr[i+1] = p1;
	      ptr[i+2] = p2;
	      ptr[i+3] = p2;
	      ptr[i+4] = p1;
	      ptr[i+5] = p1;
	      ptr[i+6] = p2;
	      ptr[i+7] = p2;
	      ptr[i+8] = p1;
	      ptr[i+9] = p1;
	      ptr[i+10] = p2;
	      ptr[i+11] = p2;
	      ptr[i+12] = p1;
	      ptr[i+13] = p1;
	      ptr[i+14] = p2;
	      ptr[i+15] = p2;

	      p1 = p1<<1;

	      if (p1 == 0){
	          p1 = 1;
	      }
    }

    return;
}


__global__ void 
kernel_test5_move(char* _ptr, char* end_ptr)
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);

    if (ptr >= (unsigned int*) end_ptr) {
	      return;
    }

    unsigned int half_count = BLOCKSIZE/sizeof(unsigned int)/2;
    unsigned int* ptr_mid = ptr + half_count;

    for (i = 0;i < half_count; i++){
	      ptr_mid[i] = ptr[i];
    }

    for (i=0;i < half_count - 8; i++){
	      ptr[i + 8] = ptr_mid[i];
    }

    for (i=0;i < 8; i++){
	      ptr[i] = ptr_mid[half_count - 8 + i];
    }

    return;
}


__global__ void 
kernel_test5_check(char* _ptr, char* end_ptr, unsigned int* err, unsigned long* ptFailedAdress,
		   unsigned long* expectedValue, unsigned long* currentError, unsigned long* ptValueOfStartAddr)
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);

    if (ptr >= (unsigned int*) end_ptr) {
	      return;
    }

    for (i=0;i < BLOCKSIZE/sizeof(unsigned int); i+=2){
	      if (ptr[i] != ptr[i+1]){
	          RECORD_ERR(err, &ptr[i], ptr[i+1], ptr[i]);
	      }
    }

    return;
}

void test5(char* ptr, unsigned int tot_num_blocks)
{

    unsigned int i;
    char* end_ptr = ptr + tot_num_blocks* BLOCKSIZE;

    for (i=0;i < tot_num_blocks; i+= GRIDSIZE){
	      dim3 grid;

	      error_checking("Intitalizing test 5 ",  i);
	      grid.x= GRIDSIZE;
        hipLaunchKernelGGL(kernel_test5_init,
                            dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                           ptr + i*BLOCKSIZE, end_ptr);
        SHOW_PROGRESS("test5[init]", i, tot_num_blocks);
    }


    for (i=0;i < tot_num_blocks; i+= GRIDSIZE){
	      dim3 grid;

	      grid.x= GRIDSIZE;
        hipLaunchKernelGGL(kernel_test5_move,
                            dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                           ptr + i*BLOCKSIZE, end_ptr);
        SHOW_PROGRESS("test5[move]", i, tot_num_blocks);
    }


    for (i=0;i < tot_num_blocks; i+= GRIDSIZE){
	      dim3 grid;

	      grid.x= GRIDSIZE;
        hipLaunchKernelGGL(kernel_test5_check,
                            dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                             ptr + i*BLOCKSIZE, end_ptr, ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr);
	      SHOW_PROGRESS("test5[check]", i, tot_num_blocks);
    }

    return;

}

/*****************************************************************************************
 * Test 6 [Moving inversions, 32 bit pat]
 * This is a variation of the moving inversions algorithm that shifts the data
 * pattern left one bit for each successive address. The starting bit position
 * is shifted left for each pass. To use all possible data patterns 32 passes
 * are required.  This test is quite effective at detecting data sensitive
 * errors but the execution time is long.
 *
 ***************************************************************************************/


  __global__ void 
kernel_movinv32_write(char* _ptr, char* end_ptr, unsigned int pattern,
		unsigned int lb, unsigned int sval, unsigned int offset)
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);

    if (ptr >= (unsigned int*) end_ptr) {
	      return;
    }

    unsigned int k = offset;
    unsigned pat = pattern;

    for (i = 0;i < BLOCKSIZE/sizeof(unsigned int); i++){
	      ptr[i] = pat;
	      k++;

	      if (k >= 32){
	          k=0;
	          pat = lb;
	      }else{
	        pat = pat << 1;
	        pat |= sval;
	      }
    }

    return;
}


__global__ void 
kernel_movinv32_readwrite(char* _ptr, char* end_ptr, unsigned int pattern,
			  unsigned int lb, unsigned int sval, unsigned int offset, unsigned int * err,
			  unsigned long* ptFailedAdress, unsigned long* expectedValue, unsigned long* currentError, unsigned long* ptValueOfStartAddr)
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);

    if (ptr >= (unsigned int*) end_ptr) {
	    return;
    }

    unsigned int k = offset;
    unsigned pat = pattern;

    for (i = 0;i < BLOCKSIZE/sizeof(unsigned int); i++){
	      if (ptr[i] != pat){
	          RECORD_ERR(err, &ptr[i], pat, ptr[i]);
	      }

	      ptr[i] = ~pat;

	      k++;

	      if (k >= 32){
	        k=0;
	        pat = lb;
	      }else{
	        pat = pat << 1;
	        pat |= sval;
	      }
    }

    return;
}



__global__ void 
kernel_movinv32_read(char* _ptr, char* end_ptr, unsigned int pattern,
		     unsigned int lb, unsigned int sval, unsigned int offset, unsigned int * err,
		     unsigned long* ptFailedAdress, unsigned long* expectedValue, unsigned long* currentError, unsigned long* ptValueOfStartAddr)
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);

    if (ptr >= (unsigned int*) end_ptr) {
	      return;
    }

    unsigned int k = offset;
    unsigned pat = pattern;

    for (i = 0;i < BLOCKSIZE/sizeof(unsigned int); i++){
	      if (ptr[i] != ~pat){
	          RECORD_ERR(err, &ptr[i], ~pat, ptr[i]);
	      }

	      k++;

	      if (k >= 32){
	        k=0;
	        pat = lb;
	      }else{
	        pat = pat << 1;
	        pat |= sval;
	      }
    }

    return;
}


void movinv32(char* ptr, unsigned int tot_num_blocks, unsigned int pattern,
	 unsigned int lb, unsigned int sval, unsigned int offset)
{

    unsigned int i;
    char* end_ptr = ptr + tot_num_blocks* BLOCKSIZE;

    for (i=0;i < tot_num_blocks; i+= GRIDSIZE){
	    dim3 grid;

	    grid.x= GRIDSIZE;
      hipLaunchKernelGGL(kernel_movinv32_write,
                            dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                           ptr + i*BLOCKSIZE, end_ptr, pattern, lb,sval, offset); 
      SHOW_PROGRESS("\nTest6[moving inversion 32 write]", i, tot_num_blocks);
    }

    for (i=0;i < tot_num_blocks; i+= GRIDSIZE){
	    dim3 grid;

	    grid.x= GRIDSIZE;
      hipLaunchKernelGGL(kernel_movinv32_readwrite,
                            dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                          ptr + i*BLOCKSIZE, end_ptr, pattern, lb,sval, offset, ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr); 
	    SHOW_PROGRESS("\nTest6[moving inversion 32 readwrite]", i, tot_num_blocks);
    }

   for (i=0;i < tot_num_blocks; i+= GRIDSIZE){
       dim3 grid;

       grid.x= GRIDSIZE;
       hipLaunchKernelGGL(kernel_movinv32_read,
                            dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
                             ptr + i*BLOCKSIZE, end_ptr, pattern, lb,sval, offset, ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr); 
	     error_checking("Test6 [movinv32]",  i);
       SHOW_PROGRESS("\nTest6[moving inversion 32 read]", i, tot_num_blocks);
   }

   return;

}

void  test6(char* ptr, unsigned int tot_num_blocks)
{
    unsigned int i;
    unsigned int pattern;

    for (i= 0, pattern = 1;i < 32; pattern = pattern << 1, i++){

        std::cout << "\nTest6[move inversion 32 bits test]: pattern ::" << std::hex << pattern << " offset=" << i;
	      movinv32(ptr, tot_num_blocks, pattern, 1, 0, i);

        std::cout << "\nTest6[move inversion 32 bits test]: pattern :: " << std::hex <<  ~pattern << " offset=" << i;
	      movinv32(ptr, tot_num_blocks, ~pattern, 0xfffffffe, 1, i);
    }
}

/******************************************************************************
 * Test 7 [Random number sequence]
 *
 * This test writes a series of random numbers into memory.  A block (1 MB) of memory
 * is initialized with random patterns. These patterns and their complements are
 * used in moving inversions test with rest of memory.
 *
 *
 *******************************************************************************/

  __global__ void 
kernel_test7_write(char* _ptr, char* end_ptr, char* _start_ptr, unsigned int* err)
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);
    unsigned int* start_ptr = (unsigned int*) _start_ptr;

    if (ptr >= (unsigned int*) end_ptr) {
	      return;
    }

    for (i = 0;i < BLOCKSIZE/sizeof(unsigned int); i++){
	      ptr[i] = start_ptr[i];
    }

    return;
}



__global__ void 
kernel_test7_readwrite(char* _ptr, char* end_ptr, char* _start_ptr, unsigned int* err,
		       unsigned long* ptFailedAdress, unsigned long* expectedValue, unsigned long* currentError, unsigned long* ptValueOfStartAddr)
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);
    unsigned int* start_ptr = (unsigned int*) _start_ptr;

    if (ptr >= (unsigned int*) end_ptr) {
	      return;
    }


    for (i = 0;i < BLOCKSIZE/sizeof(unsigned int); i++){
	      if (ptr[i] != start_ptr[i]){
	        RECORD_ERR(err, &ptr[i], start_ptr[i], ptr[i]);
	      }

	      ptr[i] = ~(start_ptr[i]);
    }

    return;
}

__global__ void 
kernel_test7_read(char* _ptr, char* end_ptr, char* _start_ptr, unsigned int* err, unsigned long* ptFailedAdress,
		  unsigned long* expectedValue, unsigned long* currentError, unsigned long* ptValueOfStartAddr)
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);
    unsigned int* start_ptr = (unsigned int*) _start_ptr;

    if (ptr >= (unsigned int*) end_ptr) {
	      return;
    }


    for (i = 0;i < BLOCKSIZE/sizeof(unsigned int); i++){
	      if (ptr[i] != ~(start_ptr[i])){
	          RECORD_ERR(err, &ptr[i], ~(start_ptr[i]), ptr[i]);
	      }
    }

    return;
}


void test7(char* ptr, unsigned int tot_num_blocks)
{

    unsigned int* host_buf = (unsigned int*)malloc(BLOCKSIZE);
    unsigned int err = 0;
    unsigned int i;
    unsigned int iteration = 0;

    
    for (i = 0;i < BLOCKSIZE/sizeof(unsigned int);i++){
      	host_buf[i] = get_random_num();
    }

    hipMemcpy(ptr, host_buf, BLOCKSIZE, hipMemcpyHostToDevice);

    char* end_ptr = ptr + tot_num_blocks* BLOCKSIZE;

    repeat:

        for (i=1;i < tot_num_blocks; i+= GRIDSIZE){
	        dim3 grid;

	        grid.x= GRIDSIZE;
          hipLaunchKernelGGL(kernel_test7_write,
                            dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                                        ptr + i*BLOCKSIZE, end_ptr, ptr, ptCntOfError); 
          SHOW_PROGRESS("test7_write", i, tot_num_blocks);
        }


        for (i=1;i < tot_num_blocks; i+= GRIDSIZE){
	        dim3 grid;

	        grid.x= GRIDSIZE;
          hipLaunchKernelGGL(kernel_test7_readwrite,
                            dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                            ptr + i*BLOCKSIZE, end_ptr, ptr, ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr);
	        err += error_checking("test7_readwrite",  i);
          SHOW_PROGRESS("test7_readwrite", i, tot_num_blocks);
        }


        for (i=1;i < tot_num_blocks; i+= GRIDSIZE){
	          dim3 grid;

	          grid.x= GRIDSIZE;
            hipLaunchKernelGGL(kernel_test7_read,
                                 dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                               ptr + i*BLOCKSIZE, end_ptr, ptr, ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr); 
	          err += error_checking("test7_read",  i);
            SHOW_PROGRESS("test7_read", i, tot_num_blocks); 
        }


        if (err == 0 && iteration == 0){
	          return;
        }

        if (iteration < MAX_ITERATION){
          std::cout << iteration << "repeating test7 because there are" << err << " errors found in last run\n";
	        iteration++;
	        err = 0;
	        goto repeat;
        }
}
/***********************************************************************************
 * Test 8 [Modulo 20, random pattern]
 *
 * A random pattern is generated. This pattern is used to set every 20th memory location
 * in memory. The rest of the memory location is set to the complimemnt of the pattern.
 * Repeat this for 20 times and each time the memory location to set the pattern is shifted right.
 *
 *
 **********************************************************************************/

__global__ void 
kernel_modtest_write(char* _ptr, char* end_ptr, unsigned int offset, unsigned int p1, unsigned int p2)
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);

    if (ptr >= (unsigned int*) end_ptr) {
	      return;
    }

    for (i = offset;i < BLOCKSIZE/sizeof(unsigned int); i+=MOD_SZ){
	      ptr[i] =p1;
    }

    for (i = 0;i < BLOCKSIZE/sizeof(unsigned int); i++){
	      if (i % MOD_SZ != offset){
	          ptr[i] =p2;
	      }
    }

    return;
}


__global__ void 
kernel_modtest_read(char* _ptr, char* end_ptr, unsigned int offset, unsigned int p1, unsigned int* err,
		    unsigned long* ptFailedAdress, unsigned long* expectedValue, unsigned long* currentError, unsigned long* ptValueOfStartAddr)
{
    unsigned int i;
    unsigned int* ptr = (unsigned int*) (_ptr + blockIdx.x*BLOCKSIZE);

    if (ptr >= (unsigned int*) end_ptr) {
	      return;
    }

    for (i = offset;i < BLOCKSIZE/sizeof(unsigned int); i+=MOD_SZ){
	      if (ptr[i] !=p1){
	        RECORD_ERR(err, &ptr[i], p1, ptr[i]);
	      }
    }

    return;
}

unsigned int modtest(char* ptr, unsigned int tot_num_blocks, unsigned int offset, unsigned int p1, unsigned int p2)
{

    char* end_ptr = ptr + tot_num_blocks* BLOCKSIZE;
    unsigned int i;
    unsigned int err = 0;

    for (i= 0;i < tot_num_blocks; i+= GRIDSIZE){
	    dim3 grid;

	    grid.x= GRIDSIZE;
      hipLaunchKernelGGL(kernel_modtest_write,
                         dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                       ptr + i*BLOCKSIZE, end_ptr, offset, p1, p2); 
      SHOW_PROGRESS("test8[mod test, write]", i, tot_num_blocks);
    }

    for (i= 0;i < tot_num_blocks; i+= GRIDSIZE){
	    dim3 grid;

	    grid.x= GRIDSIZE;
      hipLaunchKernelGGL(kernel_modtest_read,
                         dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                       ptr + i*BLOCKSIZE, end_ptr, offset, p1, ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr); 
	    err += error_checking("test8[mod test, read", i);
      SHOW_PROGRESS("test8[mod test, read]", i, tot_num_blocks);
    }

    return err;

}

void test8(char* ptr, unsigned int tot_num_blocks)
{
    unsigned int i;
    unsigned int err = 0;
    unsigned int iteration = 0;
    unsigned int p1;

    if (global_pattern){
	    p1 = global_pattern;
    }else{
	    p1= get_random_num();
    }

    unsigned int p2 = ~p1;

 repeat:

    std::cout << "test8[mod test]: p1=" << p1 << "p2= " << p2 << "\n";

    for (i = 0;i < MOD_SZ; i++){
	    err += modtest(ptr, tot_num_blocks,i, p1, p2);
    }

    if (err == 0 && iteration == 0){
	      return;
    }

    if (iteration < MAX_ITERATION){
        std::cout << iteration << "th repeating test8 because there are "<< err << "errors found in last run, p1= " << p1 << " p2= "<< p2;
	      iteration++;
	      err = 0;
	      goto repeat;
    }
}

/************************************************************************************
 *
 * Test 9 [Bit fade test, 90 min, 2 patterns]
 * The bit fade test initializes all of memory with a pattern and then
 * sleeps for 90 minutes. Then memory is examined to see if any memory bits
 * have changed. All ones and all zero patterns are used. This test takes
 * 3 hours to complete.  The Bit Fade test is disabled by default
 *
 **********************************************************************************/

void test9(char* ptr, unsigned int tot_num_blocks)
{

    unsigned int p1 = 0;
    unsigned int p2 = ~p1;

    unsigned int i;
    char* end_ptr = ptr + tot_num_blocks* BLOCKSIZE;

    for (i= 0;i < tot_num_blocks; i+= GRIDSIZE){
	    dim3 grid;

	    grid.x= GRIDSIZE;
      hipLaunchKernelGGL(kernel_move_inv_write,
                         dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                        ptr + i*BLOCKSIZE, end_ptr, p1); 
      SHOW_PROGRESS("test9[bit fade test, write]", i, tot_num_blocks);
    }

    //sleep(60*90);
    std::this_thread::sleep_for(std::chrono::milliseconds(90));

    for (i=0;i < tot_num_blocks; i+= GRIDSIZE){
	    dim3 grid;

	    grid.x= GRIDSIZE;
      hipLaunchKernelGGL(kernel_move_inv_readwrite,
                         dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                        ptr + i*BLOCKSIZE, end_ptr, p1, p2, ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr); 
	    error_checking("test9[bit fade test, readwrite]",  i);
      SHOW_PROGRESS("test9[bit fade test, readwrite]", i, tot_num_blocks);
    }

    //sleep(60*90);
    std::this_thread::sleep_for(std::chrono::milliseconds(90));

    for (i=0;i < tot_num_blocks; i+= GRIDSIZE){
	    dim3 grid;

	    grid.x= GRIDSIZE;
      hipLaunchKernelGGL(kernel_move_inv_read,
                         dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                        ptr + i*BLOCKSIZE, end_ptr, p2, ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr); 
	    error_checking("test9[bit fade test, read]",  i);
      SHOW_PROGRESS("test9[bit fade test, read]", i, tot_num_blocks);
    }

    return;
}

/**************************************************************************************
 * Test10 [memory stress test]
 *
 * Stress memory as much as we can. A random pattern is generated and a kernel of large grid size
 * and block size is launched to set all memory to the pattern. A new read and write kernel is launched
 * immediately after the previous write kernel to check if there is any errors in memory and set the
 * memory to the compliment. This process is repeated for 1000 times for one pattern. The kernel is
 * written as to achieve the maximum bandwidth between the global memory and GPU.
 * This will increase the chance of catching software error. In practice, we found this test quite useful
 * to flush hardware errors as well.
 *
 */

__global__ void  
test10_kernel_write(char* ptr, int memsize, TYPE p1)
{
    int i;
    int avenumber = memsize/(gridDim.x*gridDim.y);
    TYPE* mybuf = (TYPE*)(ptr + blockIdx.x* avenumber);
    int n = avenumber/(blockDim.x*sizeof(TYPE));

    for(i=0;i < n;i++){
        int index = i*blockDim.x + threadIdx.x;
        mybuf[index]= p1;
    }
    int index = n*blockDim.x + threadIdx.x;
    if (index*sizeof(TYPE) < avenumber){
        mybuf[index] = p1;
    }

    return;
}

__global__ void  
test10_kernel_readwrite(char* ptr, int memsize, TYPE p1, TYPE p2,  unsigned int* err,
					unsigned long* ptFailedAdress, unsigned long* expectedValue, unsigned long* currentError, unsigned long* ptValueOfStartAddr)
{
    int i;
    int avenumber = memsize/(gridDim.x*gridDim.y);
    TYPE* mybuf = (TYPE*)(ptr + blockIdx.x* avenumber);
    int n = avenumber/(blockDim.x*sizeof(TYPE));
    TYPE localp;

    for(i=0;i < n;i++){
        int index = i*blockDim.x + threadIdx.x;

        localp = mybuf[index];
        if (localp != p1){
	          RECORD_ERR(err, &mybuf[index], p1, localp);
	      }

	      mybuf[index] = p2;
    }

    int index = n*blockDim.x + threadIdx.x;

    if (index*sizeof(TYPE) < avenumber){
	      localp = mybuf[index];

	      if (localp!= p1){
	        RECORD_ERR(err, &mybuf[index], p1, localp);
	      }
	      mybuf[index] = p2;
    }

    return;
}

void test10(char* ptr, unsigned int tot_num_blocks)
{
    TYPE p1;

    if (global_pattern_long){
	      p1 = global_pattern_long;
    }else{
	      p1 = get_random_num_long();
    }

    TYPE p2 = ~p1;

    hipStream_t stream;
    hipEvent_t start, stop;

    std::cout << "\n Test10 with pattern =" << p1;
    HIP_CHECK(hipStreamCreate(&stream));
    HIP_CHECK(hipEventCreate(&start));
    HIP_CHECK(hipEventCreate(&stop));

    int n = num_iterations;
    float elapsedtime;

    dim3 gridDim(STRESS_GRIDSIZE);
    dim3 blockDim(STRESS_BLOCKSIZE);
    HIP_CHECK(hipEventRecord(start, stream));

    hipLaunchKernelGGL(test10_kernel_write,
                         dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
                          ptr, tot_num_blocks*BLOCKSIZE, p1); 

    for(int i =0;i < n ;i ++){
                    hipLaunchKernelGGL(test10_kernel_readwrite,
                         dim3(blocks), dim3(threadsPerBlock), 0/*dynamic shared*/, 0/*stream*/,     /* launch config*/
	                        ptr, tot_num_blocks*BLOCKSIZE, p1, p2,
								          ptCntOfError, ptFailedAdress, expectedValue, ptCurrentValue, ptValueOfStartAddr); 
	        p1 = ~p1;
	        p2 = ~p2;
    }

    hipEventRecord(stop, stream);
    hipEventSynchronize(stop);

    error_checking("test10[Memory stress test]",  0);
    hipEventElapsedTime(&elapsedtime, start, stop);
    std::cout << " \n Test10: elapsedtime = " << elapsedtime << " bandwidth = " << (2*n+1)*tot_num_blocks/elapsedtime << "GB/s \n";

    hipEventDestroy(start);
    hipEventDestroy(stop);

    hipStreamDestroy(stream);
}


////////////////////////////////////////////////////////////////////////////////
// These are hip Helper functions

void allocate_small_mem(void)
{
    //Initialize memory
    HIP_CHECK(hipMalloc((void**)&ptCntOfError, sizeof(unsigned int))); 
    HIP_CHECK(hipMemset(ptCntOfError, 0, sizeof(unsigned int))); 

    HIP_CHECK(hipMalloc((void**)&ptFailedAdress, sizeof(unsigned long) * MAX_ERR_RECORD_COUNT));
    HIP_CHECK(hipMemset(ptFailedAdress, 0, sizeof(unsigned long) * MAX_ERR_RECORD_COUNT));

    HIP_CHECK(hipMalloc((void**)&expectedValue, sizeof(unsigned long) * MAX_ERR_RECORD_COUNT));
    HIP_CHECK(hipMemset(expectedValue, 0, sizeof(unsigned long) * MAX_ERR_RECORD_COUNT));

    HIP_CHECK(hipMalloc((void**)&ptCurrentValue, sizeof(unsigned long) * MAX_ERR_RECORD_COUNT));
    HIP_CHECK(hipMemset(ptCurrentValue, 0, sizeof(unsigned long) * MAX_ERR_RECORD_COUNT));

    HIP_CHECK(hipMalloc((void**)&ptValueOfStartAddr, sizeof(unsigned long) * MAX_ERR_RECORD_COUNT));
    HIP_CHECK(hipMemset(ptValueOfStartAddr, 0, sizeof(unsigned long) * MAX_ERR_RECORD_COUNT));

    HIP_CHECK(hipMalloc((void**)&ptDebugValue, sizeof(unsigned int))); 
    HIP_CHECK(hipMemset(ptDebugValue, 0, sizeof(unsigned int))); 
}

void free_small_mem(void)
{
    //Initialize memory
    HIP_CHECK(hipFree((void*)ptFailedAdress));

    HIP_CHECK(hipFree((void*)expectedValue));

    HIP_CHECK(hipFree((void*)ptCurrentValue));

    HIP_CHECK(hipFree((void*)ptValueOfStartAddr));

    HIP_CHECK(hipFree((void*)ptDebugValue));
}



rvs_memtest_t rvs_memtests[]={
    {test0, (char*)"Test0 [Walking 1 bit]",			1},
    {test1, (char*)"Test1 [Own address test]",			1},
    {test2, (char*)"Test2 [Moving inversions, ones&zeros]",	1},
    {test3, (char*)"Test3 [Moving inversions, 8 bit pat]",	1},
    {test4, (char*)"Test4 [Moving inversions, random pattern]",1},
    {test5, (char*)"Test5 [Block move, 64 moves]",		1},
    {test6, (char*)"Test6 [Moving inversions, 32 bit pat]",	1},
    {test7, (char*)"Test7 [Random number sequence]",		1},
    {test8, (char*)"Test8 [Modulo 20, random pattern]",	1},
    {test9, (char*)"Test9 [Bit fade test]",			0},
    {test10, (char*)"Test10 [Memory stress test]",		1},
};

void list_tests_info(void)
{
    size_t i;

    for (i = 0;i < DIM(rvs_memtests); i++){
	          printf("%s %s\n", rvs_memtests[i].desc, rvs_memtests[i].enabled?"":" ==disabled by default==");
    }

    return;
}


void usage(char** argv)
{

    char example_usage[] =
	      "run on default setting:       ./rvs_memtest\n"
	      "run on stress test only:      ./rvs_memtest --stress\n";

    printf("Usage:%s [options]\n", argv[0]);
    printf("options:\n");
    printf("--mappedMem                 run all checks with rvs mapped memory instead of native device memory\n");
    printf("--silent                    Do not print out progress message (default)\n");
    printf("--device <idx>              Designate one device for test\n");
    printf("--interactive               Progress info will be printed in the same line\n");
    printf("--disable_all               Disable all tests\n");
    printf("--enable_test <test_idx>    Enable the test <test_idx>\n");
    printf("--disable_test <test_idx>   Disable the test <test_idx>\n");
    printf("--max_num_blocks <n>        Set the maximum of blocks of memory to test\n");
    printf("                            1 block = 1 MB in here\n");
    printf("--exit_on_error             When finding error, print error message and exit\n");
    printf("--monitor_temp <interval>   Monitoring temperature, the temperature will be updated every <interval> seconds\n");
    printf("                            This feature is experimental\n");
    printf("--emails <a@b,c@d,...>      Setting email notification\n");
    printf("--report_interval <n>       Setting the interval in seconds between email notifications(default 1800)\n");
    printf("--pattern <pattern>         Manually set test pattern for test4/test8/test10\n");
    printf("--list_tests                List all test descriptions\n");
    printf("--num_iterations <n>        Set the number of iterations (only effective on test0 and test10)\n");
    printf("--num_passes <n>            Set the number of test passes (this affects all tests)\n");
    printf("--verbose <n>               Setting verbose level\n");
    printf("                              0 -- Print out test start and end message only (default)\n");
    printf("                              1 -- Print out pattern messages in test\n");
    printf("                              2 -- Print out progress messages\n");
    printf("--stress                    Stress test. Equivalent to --disable_all --enable_test 10 --exit_on_error\n");
    printf("--help                      Print this message\n");
    printf("\nExample usage:\n\n");
    printf("%s\n", example_usage);

    exit(ERR_GENERAL);
}

void run_tests(char* ptr, unsigned int tot_num_blocks)
{
    struct timeval  t0, t1;
    unsigned int pass = 0;
    unsigned int i;

    blocks = 512;
    threadsPerBlock = 256;
    num_iterations = 1;

    for(int n = 0; n < num_iterations; n++) {
        for (i = 0;i < DIM(rvs_memtests); i++){
            gettimeofday(&t0, NULL);
            rvs_memtests[i].func(ptr, tot_num_blocks);
            gettimeofday(&t1, NULL);

        }//for
        hipDeviceReset(); 
        std::cout << "\n Memory tests :: " << std::dec << i << " tests complete \n ";
    }
}

