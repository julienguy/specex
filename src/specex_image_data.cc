#include <iostream>
#include <cstdio>
#include <string>
#include <cmath>

#include <specex_unbls.h>

#include <specex_image_data.h>
#include <specex_linalg.h>

using namespace std;

specex::image_data::image_data() : 
  image_data_base()
{
 rows_=0;
 cols_=0;
}

specex::image_data::image_data(size_t ncols, size_t nrows) : 
  image_data_base()
{
  resize(ncols,nrows);
}

specex::image_data::image_data(size_t ncols, size_t nrows, const unbls::vector_double& i_data) : 
  image_data_base()
{
  resize(ncols,nrows);
  data = i_data; // copy
}

void specex::image_data::resize(size_t ncols, size_t nrows) {
  rows_ = nrows;
  cols_ = ncols;
  data.resize(rows_*cols_);
  unbls::zero(data);
}




