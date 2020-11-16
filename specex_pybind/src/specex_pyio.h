#ifndef SPECEX_PYIO__H
#define SPECEX_PYIO__H

#include <boost/program_options.hpp>
#include <vector>
#include <string>

#include <harp.hpp>

#include <specex_desi_io.h>

#include <specex_pyoptions.h>
#include <specex_pyimage.h>
#include <specex_pypsf.h>

namespace specex {
  
  class PyIO : public std::enable_shared_from_this <PyIO> {

  public :

    typedef std::shared_ptr <PyIO> pshr;

    bool use_input_specex_psf;
    bool psf_change_req;

    int check_input_psf(specex::PyOptions);
    int read_img_data( specex::PyOptions, specex::PyImage&);
    int read_psf_data( specex::PyOptions, specex::PyPSF&  );
    int write_psf_data(specex::PyOptions, specex::PyPSF&  );
    int write_spots(   specex::PyOptions, specex::PyPSF&  );
    int read_img_datam(specex::PyOptions,
		       specex::image_data&,
		       specex::image_data&,
		       specex::image_data&,
		       specex::image_data&,
		       std::map<std::string,std::string>& 
		       );
    
    PyIO()
      : use_input_specex_psf(false)
      , psf_change_req(false)
      {}
    
  };
  
}

#endif