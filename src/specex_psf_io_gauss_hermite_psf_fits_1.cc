#include <fstream>
#include <boost/algorithm/string.hpp>
#include <unhrp.h>
#include <specex_fits.h>
#include <specex_trace.h>
#include <specex_gauss_hermite_psf.h>

using namespace std ;

void write_gauss_hermite_psf_fits_version_1(const specex::GaussHermitePSF& psf, fitsfile* fp, int first_hdu) {
   
  ////////////////////////////
  string PSFVER = "1";
  ////////////////////////////
  
  int NPIX_X = psf.ccd_image_n_cols;
  int NPIX_Y = psf.ccd_image_n_rows;
  int BUNDLMIN = 0;
  int BUNDLMAX = 0;
  int FIBERMIN = 0;
  int FIBERMAX = 0; 
  int NFIBERS=0;
  for(std::map<int,specex::PSF_Params>::const_iterator bundle_it = psf.ParamsOfBundles.begin();
      bundle_it != psf.ParamsOfBundles.end(); ++bundle_it) {
    
    if(bundle_it == psf.ParamsOfBundles.begin()) {
      BUNDLMIN = bundle_it->second.bundle_id;
      BUNDLMAX = bundle_it->second.bundle_id;
      FIBERMIN = bundle_it->second.fiber_min;
      FIBERMAX = bundle_it->second.fiber_max;
    }
    
    BUNDLMIN = min(BUNDLMIN,bundle_it->second.bundle_id);
    BUNDLMAX = max(BUNDLMAX,bundle_it->second.bundle_id);
    FIBERMIN = min(FIBERMIN,bundle_it->second.fiber_min);
    FIBERMAX = min(FIBERMAX,bundle_it->second.fiber_max);
  
    NFIBERS += (bundle_it->second.fiber_max-bundle_it->second.fiber_min+1);
  }
  
  int status = 0;
  fits_movabs_hdu ( fp, first_hdu, NULL, &status ); harp::fits::check ( status );
  
  
  
  // get largest legendre degree of all params in all bundles and check param numbers match !!
  int ncoeff=0;
  int nparams=psf.LocalNAllPar();
  
  for(std::map<int,specex::PSF_Params>::const_iterator bundle_it = psf.ParamsOfBundles.begin();
      bundle_it != psf.ParamsOfBundles.end(); ++bundle_it) {

    if( int(bundle_it->second.AllParPolXW.size()) != nparams ) SPECEX_ERROR("Fatal inconsistency in number of parameters in psf between bundles: AllParPolXW.size=" << bundle_it->second.AllParPolXW.size() << " psf.LocalNAllPar()=" << nparams);

    for(size_t p=0;p<bundle_it->second.AllParPolXW.size();p++) {
      ncoeff=max(ncoeff,bundle_it->second.AllParPolXW[p]->ydeg+1);
    }
  }
  
  specex::image_data image;
  vector<string> keys;

  int LEGWMIN=1000000;
  int LEGWMAX=0;

  int nparams_all = nparams;
  nparams_all += 1; // GH trivial order zero
#ifdef EXTERNAL_TAIL
  nparams_all += 5; // tail params
#endif

  { // data
    image = specex::image_data(ncoeff*(nparams_all),NFIBERS);
    image.data.clear();
    
    // now loop on real psf parameters

    unhrp::vector_double wave(ncoeff);
    unhrp::vector_double values(ncoeff);
    
    // get the max range of wavelength and convert to int 
    
    for(std::map<int,specex::PSF_Params>::const_iterator bundle_it = psf.ParamsOfBundles.begin();
	  bundle_it != psf.ParamsOfBundles.end(); ++bundle_it) {
      const specex::PSF_Params & params_of_bundle = bundle_it->second;
      for(int p=0;p<nparams;p++) { 
	LEGWMIN=min(LEGWMIN,int(floor(params_of_bundle.AllParPolXW[p]->ymin)));
	LEGWMAX=max(LEGWMAX,int(floor(params_of_bundle.AllParPolXW[p]->ymax))+1);
      }
    }
    
    
    double wavemin = double(LEGWMIN);
    double wavemax = double(LEGWMAX);

    bool need_to_add_first_gh = true;
    for(int p=0;p<nparams;p++) { 
      string pname = psf.ParamName(p);
      if(need_to_add_first_gh && pname.find("GH-")<pname.npos) { // insert now GH param 0
	// first gauss-hermite param
	
	keys.push_back("GH-0-0"); for(int f=0;f<NFIBERS;f++) image(0,f)=1;	
	need_to_add_first_gh = false;
      }

      keys.push_back(pname);
      
      int fiber_index=0;
      for(std::map<int,specex::PSF_Params>::const_iterator bundle_it = psf.ParamsOfBundles.begin();
	  bundle_it != psf.ParamsOfBundles.end(); ++bundle_it) {
	const specex::PSF_Params & params_of_bundle = bundle_it->second;
	
	const specex::Pol_p pol2d = params_of_bundle.AllParPolXW[p]; // this is the 2D polynomiald of x_ccd and wave for this param and bundle
	

	
	for(int fiber=params_of_bundle.fiber_min; fiber<=params_of_bundle.fiber_max; fiber++,fiber_index++) {
	  
	  const specex::Trace& trace = psf.FiberTraces.find(fiber)->second; // X_vs_W.Value();

	  // build a Legendre1DPol out of the Legendre2DPol
	  specex::Legendre1DPol pol1d(ncoeff-1,wavemin,wavemax);
	  double wavestep = (wavemax-wavemin)/(ncoeff-1);
	  for(int w=0;w<ncoeff;w++) {
	    wave[w]   = wavemin + wavestep*w;
	    values[w] = pol2d->Value(trace.X_vs_W.Value(wave[w]),wave[w]);
	  }
	  pol1d.Fit(wave,values,0,false);

	  // now copy parameters;
	  
	  for(int w = 0; w < ncoeff ; w++) {
	    image((p+1)*ncoeff+w,fiber_index) = pol1d.coeff[w]; // this is the definition of the ordering, (wave,fiber)
	  }
	  
	} // end of loop on fibers of bundle
	      
      } // end of loop on bundles
    } // end of loop on params
    
  }
  
  // write image
  {
    harp::fits::img_append < double > ( fp, image.n_rows(), image.n_cols() );
    harp::fits::img_write ( fp, image.data , false);
  }
  
  // write keywords
  {
    fits_write_comment(fp,"PSF generated by specex, https://github.com/julienguy/specex",&status); harp::fits::check ( status );
    
    {
      char date_comment[80];
      time_t t = time(0);   // get time now
      struct tm * now = localtime( & t );
      sprintf(date_comment,"PSF fit date %04d-%02d-%02d",(now->tm_year + 1900),(now->tm_mon + 1),now->tm_mday);
      fits_write_comment(fp,date_comment,&status); harp::fits::check ( status );
    }
    ///////////////////////xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx////////
    fits_write_comment(fp,"Each row of the image contains the PSF parameters of a fiber in the form",&status); harp::fits::check ( status );
    fits_write_comment(fp,"of Legendre coefficients. The coefficients of a given parameter are",&status); harp::fits::check ( status );
    fits_write_comment(fp,"contiguous in a row of the image. ",&status); harp::fits::check ( status );
    fits_write_comment(fp,"LEGDEG gives the degree of Legendre pol (LEGDEG+1 coeffs. per parameter)",&status); harp::fits::check ( status );
    fits_write_comment(fp,"LEGWMIN and LEGWMAX are needed to compute reduced wavelength.",&status); harp::fits::check ( status );
    fits_write_comment(fp,"PXXX(wave) = Legendre(2*(wave-LEGWMIN)/(LEGWMAX-LEGWMIN)-1,COEFFXXX)" ,&status); harp::fits::check ( status );
    fits_write_comment(fp,"The definition of the parameter XXX is given by the key word PXXX.",&status); harp::fits::check ( status );
    fits_write_comment(fp,"The number of parameters is given by the key word NPARAMS,",&status); harp::fits::check ( status );
    fits_write_comment(fp,"and the number of fibers = NAXIS2 = FIBERMAX-FIBERMIN+1",&status); harp::fits::check ( status );
    
    
    harp::fits::key_write(fp,"MJD",(long long int)psf.mjd,"MJD of arc lamp exposure");
    harp::fits::key_write(fp,"PLATEID",psf.plate_id,"plate ID of arc lamp exposure");
    harp::fits::key_write(fp,"CAMERA",psf.camera_id,"camera ID");
    
    harp::fits::key_write(fp,"PSFTYPE","GAUSS-HERMITE","");
    harp::fits::key_write(fp,"PSFVER",PSFVER,"");
    harp::fits::key_write(fp,"ARCEXP",psf.arc_exposure_id,"ID of arc lamp exposure used to fit PSF");
    
    harp::fits::key_write(fp,"NPIX_X",(long long int)NPIX_X,"number of columns in input CCD image");
    harp::fits::key_write(fp,"NPIX_Y",(long long int)NPIX_Y,"number of rows in input CCD image");
    harp::fits::key_write(fp,"BUNDLMIN",(long long int)BUNDLMIN,"first bundle of fibers (starting at 0)");
    harp::fits::key_write(fp,"BUNDLMAX",(long long int)BUNDLMAX,"last bundle of fibers (included)");
    harp::fits::key_write(fp,"FIBERMIN",(long long int)FIBERMIN,"first fiber (starting at 0)");
    harp::fits::key_write(fp,"FIBERMAX",(long long int)FIBERMAX,"last fiber (included)");
    harp::fits::key_write(fp,"NPARAMS",(long long int)keys.size(),"number of PSF parameters");
    harp::fits::key_write(fp,"LEGDEG",(long long int)(ncoeff-1),"degree of Legendre pol.(wave) for parameters");
    harp::fits::key_write(fp,"LEGWMIN",(long long int)LEGWMIN,"min. wave (A) for Legendre pol.");
    harp::fits::key_write(fp,"LEGWMAX",(long long int)LEGWMAX,"max. wave (A) for Legendre pol.");
    // harp::fits::key_write(fp,"NFIBERS",(long long int)NFIBERS,"number of fibers");
    
    // write first dummy GH param
    char keyname[8];
    char comment[80];
    for(size_t k=0;k<keys.size(); k++) {
      sprintf(keyname,"P%03d",int(k));
      sprintf(comment,"Param. Leg. coeff in cols %d-%d (start. at 0)",int(k*ncoeff),int((k+1)*ncoeff-1));
      harp::fits::key_write(fp,keyname,keys[k].c_str(),comment);
    }
  }
  

}
 
