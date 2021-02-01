#ifndef SPECEX_PYPSF__H
#define SPECEX_PYPSF__H

#include <boost/program_options.hpp>
#include <vector>
#include <string>

#include <harp.hpp>

#include <specex_psf.h>
#include <specex_spot.h>

#include <specex_gauss_hermite_psf.h>

#include <specex_pyoptions.h>

namespace specex {
  
  class PyPSF : public std::enable_shared_from_this <PyPSF> {

  public :

    typedef std::shared_ptr <PyPSF> pshr;

    specex::PSF_p psf;
    vector<Spot_p> fitted_spots;

    PyPSF(){}

    specex::image_data get_trace(std::string);
    
    int trace_ncoeff, table_nrows, nfibers;
    int FIBERMIN, FIBERMAX;
    double trace_WAVEMIN, trace_WAVEMAX;
    double table_WAVEMIN, table_WAVEMAX;
    long long mjd, plate_id, arc_exposure_id, NPIX_X, NPIX_Y,
      hSizeX, hSizeY, nparams_all, ncoeff, GHDEGX, GHDEGY;
    std::string camera_id;
    double psf_error, readout_noise, gain;
    std::vector<int>    bundle_id, bundle_ndata, bundle_nparams;
    std::vector<double> bundle_chi2pdf;

    void get_table(std::vector<std::string> &table_string,
		   std::vector<std::vector<int>> &table_int,
		   std::vector<std::vector<double>> &table_double);

    void SetParamsOfBundle();
    
  };
  
}

#endif