#ifdef USE_MKL
#include <mkl.h>
#else
#include <cblas.h>
#endif

// contains all calls to C-interface BLAS functions
// e.g. http://www.netlib.org/lapack/explore-html/d1/dff/cblas__example1_8c_source.html

// returns the dot product of x and y
// http://www.netlib.org/lapack/explore-html/d5/df6/ddot_8f_source.html
double specex_dot(int n, const double *x, const double *y){
  return cblas_ddot(n, x, 1, y, 1);
}

// y += alpha*x
// http://www.netlib.org/lapack/explore-html/d9/dcd/daxpy_8f_source.html
void specex_axpy(int n, const double *alpha, const double *x, const double *y){
  cblas_daxpy(n, *alpha, x, 1, y, 1);  
}

// A += alpha*x*x**T, where A is a symmetric matrx (only lower half is filled)
// http://www.netlib.org/lapack/explore-html/d3/d60/dsyr_8f_source.html
void specex_syr(int n, const double *alpha, const double *x, const double *A){
  cblas_dsyr(CblasColMajor, CblasLower, n, *alpha, x, 1, A, n);  
}

// C = alpha*A**T + beta*C, where A is a symmetric matrx 
// http://www.netlib.org/lapack/explore-html/dc/d05/dsyrk_8f_source.html
void specex_syrk(int n, int k, const double *alpha, const double *A, const double *beta,
		 const double *C){
  cblas_dsyrk(CblasColMajor, CblasLower, CblasNoTrans, n, k, *alpha, A, n, *beta, C, n);  
}

// y = alpha*A*x + beta*y
// http://www.netlib.org/lapack/explore-html/dc/da8/dgemv_8f_source.html
void specex_gemv(int m, int n, const double *alpha, const double *A, const double *x,
		 const double *beta, const double *y){
  cblas_dgemv(CblasColMajor, CblasNoTrans, m, n, *alpha, A, m, x, 1, *beta, y, 1);
  
}

// C = alpha*A*B + beta*C
// http://www.netlib.org/lapack/explore-html/d7/d2b/dgemm_8f_source.html
void specex_gemm(int m, int n, int k, const double *alpha, const double *A, const double *B,
		 const double *beta, const double *C){
  cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, m, n, k, *alpha, A, m, B, k, *beta, C, m);  
}

