#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include <map>

#include <boost/program_options.hpp>

#include <boost/archive/xml_oarchive.hpp>

#include <harp.hpp>

#include <specex_message.h>
#include <specex_linalg.h>
#include <specex_psf.h>
#include <specex_trace.h>
#include <specex_fits.h>
#include <specex_boss_io.h>
#include <specex_psf_fitter.h>
#include <specex_psf_io.h>
#include <specex_serialization.h>

using namespace std;

namespace popts = boost::program_options;

#define _GNU_SOURCE 1
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <fenv.h>

specex::PSF_p psf;
specex::image_data image,weight;
specex::Legendre2DPol dxpol;
std::vector<int> rows;
harp::vector_double flux;
int degx,degw,nrows,nfibers,npardx,npar;
harp::matrix_double A;
harp::vector_double B;
int hsizex;

int segment_index(int afiber, int aj)  {
  return afiber*10000+aj;
}

class Segment {
public :
  int fiber;
  int j;
  int begin_i;
  int end_i;
  int npix;
  double chi2;
  double flux;
  double weight;
  Segment() {
    chi2=0;
    npix=0;
    flux=0;
    weight=0;
  }
  int fiber_from_index(int index) const {
    return index/10000;
  }
  int j_from_index(int index) const {
    return index%10000;
  }
  int index() const {
    return segment_index(fiber,j);
  }
};

std::map<int,Segment> segments; // use above index to find them
bool compute_segments;


void robustify() {
  SPECEX_INFO("robustify");
  
  std::map<int,double> chi2pdf_of_row;
  std::map<int,int> n_segments_of_row;
  
  for(int jj=0;jj<nrows;jj++) {
    chi2pdf_of_row[rows[jj]]=0.;
    n_segments_of_row[rows[jj]]=0;
  }
    
  // normalize chi2 per row
  for(std::map<int,Segment>::iterator it=segments.begin(); it!=segments.end(); it++) {
    Segment &seg=it->second;
    if(seg.npix>1) {
      seg.chi2/=(seg.npix-1);
      seg.flux/=seg.weight;
      chi2pdf_of_row.find(seg.j)->second += seg.chi2;
      n_segments_of_row.find(seg.j)->second ++;
    }
  }
  for(int jj=0;jj<nrows;jj++) {
    int n=n_segments_of_row.find(rows[jj])->second;
    if(n>0)
      chi2pdf_of_row.find(rows[jj])->second /= n;
    cout << "j=" << rows[jj] << " chi2pdf=" << chi2pdf_of_row.find(rows[jj])->second << endl;
  }
  
  for(std::map<int,Segment>::iterator it=segments.begin(); it!=segments.end(); it++) {
    Segment &seg=it->second;
    seg.chi2/=chi2pdf_of_row.find(seg.j)->second;
  }
  
  if(1) {
    ofstream os("seg.list");
    os << "# fiber :" << endl;
    os << "# j :" << endl;
    os << "# i1 :" << endl;
    os << "# i2 :" << endl;
    os << "# chi2 :" << endl;
    os << "# npix :" << endl;
    os << "# flux :" << endl;
    os << "# weight :" << endl;
    os << "#end" << endl;
    
    for(std::map<int,Segment>::const_iterator it=segments.begin(); it!=segments.end(); it++) {
      const Segment &seg=it->second;
      os << seg.fiber << " " << seg.j << " " << seg.begin_i << " " << seg.end_i << " " << seg.chi2 << " " << seg.npix << " " << seg.flux << " " << seg.weight << endl;
    }
    os.close();
  }
  
  for(std::map<int,Segment>::iterator it=segments.begin(); it!=segments.end(); it++) {
    Segment &seg=it->second;
    if(seg.chi2>5) {
      for(int i=seg.begin_i;i<seg.end_i;i++)
	weight(i,seg.j)=0;
    }
  }
  
}




double computechi2ab(bool fit_flux, bool fit_dx) {
  npardx = 0;
  npar   = 0;
  if(fit_dx) { npardx=(degx+1)*(degw+1); npar += npardx;}
  if(fit_flux) { npar += nrows*nfibers;}
  cout << "npardx=" << npardx << endl;
  cout << "npar  =" << npar << endl;
  
  harp::vector_double H;
  harp::vector_double pos_der;
  
  if(compute_segments) {
    segments.clear(); 
  }
  
  if(fit_dx || fit_flux) {
    A.resize(npar,npar); A.clear();
    B.resize(npar); B.clear();
    H.resize(npar); 
    pos_der.resize(2);
  }
  
  double chi2=0;
  int global_flux_index = 0;
  
  for(std::map<int,specex::PSF_Params>::iterator bundle_it = psf->ParamsOfBundles.begin(); bundle_it != psf->ParamsOfBundles.end(); bundle_it++) {
    
    const specex::PSF_Params& params_of_bundle = bundle_it->second;
    
    if(params_of_bundle.bundle_id%5==0) cout << "doing bundle " << params_of_bundle.bundle_id << endl;
    
    for(int jj=0;jj<nrows;jj++) {
      int j = rows[jj];
      
      int global_begin_i = max(0,int(floor(psf->GetTrace(params_of_bundle.fiber_min).X_vs_Y.Value(j)))-hsizex);
      int global_end_i   = min(int(image.n_cols()),int(floor(psf->GetTrace(params_of_bundle.fiber_max).X_vs_Y.Value(j)))+hsizex+1);
      
      // precompute things
      std::map<int,double> wave_of_fiber;
      std::map<int,double> xcenter_of_fiber;
      std::map<int,harp::vector_double> psf_params_of_fiber;
      std::map<int,harp::vector_double> dxmonomials_of_fiber;
      
      for(int fiber=params_of_bundle.fiber_min; fiber<=params_of_bundle.fiber_max; fiber++) {
	const specex::Trace& trace = psf->GetTrace(fiber);
	if(trace.Off()) continue; 
	wave_of_fiber[fiber]        = trace.W_vs_Y.Value(j);
	xcenter_of_fiber[fiber]     = trace.X_vs_W.Value(wave_of_fiber[fiber]);
	xcenter_of_fiber[fiber]     += dxpol.Value(xcenter_of_fiber[fiber],wave_of_fiber[fiber]);
	psf_params_of_fiber[fiber]  = psf->AllLocalParamsXW(xcenter_of_fiber[fiber],wave_of_fiber[fiber],params_of_bundle.bundle_id);
	if(fit_dx)
	  dxmonomials_of_fiber[fiber] = dxpol.Monomials(xcenter_of_fiber[fiber],wave_of_fiber[fiber]);
	
	if(compute_segments) {
	  Segment segment;
	  segment.fiber=fiber;
	  segment.j=j;
	  segment.begin_i = max(0,int(floor(xcenter_of_fiber[fiber]))-hsizex);
	  segment.end_i   = min(int(image.n_cols()),int(floor(xcenter_of_fiber[fiber]))+hsizex+1);
	  segments[segment.index()]=segment;
	}
      }
      
      
      int flux_index=0;
      for(int i=global_begin_i;i<global_end_i;i++) {
	
	double w=weight(i,j);
	if(w==0) continue;
	double res = image(i,j);
	if(fit_dx || fit_flux) H.clear();
	
	bool participate=false;
	
	flux_index = global_flux_index-1;
	
	for(int fiber=params_of_bundle.fiber_min; fiber<=params_of_bundle.fiber_max; fiber++) {
	  const specex::Trace& trace = psf->GetTrace(fiber);
	  if(trace.Off()) continue; 
	  flux_index++;
	    
	  const double& xcenter = xcenter_of_fiber[fiber];
	  int begin_i = max(0,int(floor(xcenter))-hsizex);
	  int end_i   = min(int(image.n_cols()),int(floor(xcenter))+hsizex+1);
	  if(i<begin_i || i>=end_i) continue;
	  
	  // compute profile
	  
	  double prof = 0;
	  if(fit_dx) {
	    pos_der.clear();
	    prof = psf->PSFValueWithParamsXY(xcenter,double(j),i,j,psf_params_of_fiber[fiber],&pos_der,NULL,true,true);
	  }else{
	    prof = psf->PSFValueWithParamsXY(xcenter,double(j),i,j,psf_params_of_fiber[fiber],NULL,NULL,true,true);
	  }
	  //cout << "prof = " << prof << endl;
	  
	  if(prof) {
	  
	    res -= flux(flux_index)*prof;
	    
	    if(fit_flux) H(npardx+flux_index) += prof;
	    if(fit_dx) ublas::noalias(ublas::project(H,ublas::range(0,npardx))) += (pos_der(0) * flux(flux_index))*dxmonomials_of_fiber[fiber];
	  
	    participate=true;
	  }
	  
	} // end of loop on fibers 
	
	

	if(participate) {
	  /*
	    cout << "H=" << H << endl;
	    cout << "w=" << w << endl;
	    cout << "res=" << res << endl;
	    
	    exit(12);
	  */
	  chi2 += w*res*res;
	  if(fit_dx || fit_flux) {
	    specex::syr(w,H,A);
	    specex::axpy(w*res,H,B);
	  }
	 

	  if(compute_segments) {
	    for(int fiber=params_of_bundle.fiber_min; fiber<=params_of_bundle.fiber_max; fiber++) {
	      const specex::Trace& trace = psf->GetTrace(fiber);
	      if(trace.Off()) continue;
	      Segment &segment = segments.find(segment_index(fiber,j))->second;
	      if(i<segment.begin_i || i>=segment.end_i) continue;
	      segment.chi2 += w*res*res;
	      segment.flux += w*(-res+image(i,j));
	      segment.weight += w;	      
	      segment.npix += 1;
	    }
	  }
	  
	  
	} 
      } // end of loop on columns 
      
      global_flux_index = flux_index;
      
    } // end of loop on rows
     
  }// end of loop on fiber bundles

  
  return chi2;
}



int main ( int argc, char *argv[] ) {
  
  // to crash when NaN
  feenableexcept (FE_INVALID|FE_DIVBYZERO|FE_OVERFLOW);
  

  // default arguments
  // --------------------------------------------
  string input_flat_image_filename = "";
  string input_psf_xml_filename = "";
  string output_psf_xml_filename = "";
  string output_psf_fits_filename = "";
  
  string spectrograph_name = "BOSS";
  

  
  // reading arguments
  // --------------------------------------------
  popts::options_description desc ( "Allowed Options" );
  desc.add_options()
    ( "help,h", "display usage information" )
    ( "flat", popts::value<string>( &input_flat_image_filename ), "fiber flat pre-reduced fits image file name (mandatory), ex:  sdProc-b1-00108382.fits" )
    ( "psf", popts::value<string>( &input_psf_xml_filename ), "input psf xml filename" )
    ( "xml", popts::value<string>( &output_psf_xml_filename), "output psf xml filename" )
    ( "fits", popts::value<string>( &output_psf_fits_filename), "output psf fits filename" )
    ;

  popts::variables_map vm;
  
  try {
    
    popts::store(popts::command_line_parser( argc, argv ).options(desc).run(), vm);
    popts::notify(vm);
    
    if ( ( argc < 2 ) || vm.count( "help" ) || ( ! vm.count( "flat" ) )  || ( ! vm.count( "psf" ) ) || ( ! vm.count( "xml" ) ) ) {
      cerr << endl;
      cerr << desc << endl;
      return EXIT_FAILURE;
    }
  }catch(std::exception e) {
    cerr << "error in arguments" << endl;
    cerr << endl;
    cerr << desc << endl;
    return EXIT_FAILURE;
  }

  specex_set_verbose(true);
  specex_set_dump_core(true);

  // read PSF
  // --------------------------------------------
  
  {
    std::ifstream is(input_psf_xml_filename.c_str());
    boost::archive::xml_iarchive xml_ia ( is );
    xml_ia >> BOOST_SERIALIZATION_NVP(psf);
    is.close();
  }

  
  

  
  // read image
  // --------------------------------------------
  
  read_fits_images(input_flat_image_filename,image,weight);
  
  // allocation of the correction we want to fit
  // --------------------------------------------
  
  // parameters :
  degx=2;
  degw=2;
  nrows=10; // number of rows of the ccd we are going to look at
  hsizex=4;

  dxpol=specex::Legendre2DPol(degx,0,image.n_cols(),degw,0,image.n_rows());
  
  
  // count fibers and define y range
  int jmin=image.n_rows();
  int jmax=0;
  nfibers = 0;
  for(std::map<int,specex::PSF_Params>::iterator bundle_it = psf->ParamsOfBundles.begin(); bundle_it != psf->ParamsOfBundles.end(); bundle_it++) {
    for(int fiber=bundle_it->second.fiber_min; fiber<=bundle_it->second.fiber_max; fiber++) {
      const specex::Trace& trace = psf->GetTrace(fiber);
      if(trace.Off()) continue; 
      nfibers++; 
      jmin=min(jmin,int(trace.Y_vs_W.Value(trace.Y_vs_W.xmin)));
      jmax=max(jmax,int(trace.Y_vs_W.Value(trace.Y_vs_W.xmax)));
    }
  }
  cout << "ymin ymax = " << jmin << " " << jmax << endl;

  
  rows.resize(nrows);
  for(int i=0;i<nrows;i++) {
    rows[i]=jmin+(i*(jmax-jmin))/(nrows-1);
    if(i==nrows-1)
      rows[i]=jmax;
    //cout << i << " " << rows[i] << endl;
  }
  
  
  flux.resize(nrows*nfibers); flux.clear();
  
  compute_segments=false;
  int nmaxiter=20;
  for(int iter=0;iter<nmaxiter;iter++) {
    
    bool fit_flux = (iter%2==0); 
    bool fit_dx   = !fit_flux;
    
    SPECEX_INFO("fill matrix iter #" <<iter);
    
    double previous_chi2 = computechi2ab(fit_flux,fit_dx);
    
    for(int p=0;p<npar;p++)
      if(A(p,p)==0) A(p,p)=1.e-12;
    
    //specex::write_new_fits_image("A.fits",A); SPECEX_WARNING("wrote A.fits");
    SPECEX_INFO("solving iter #" <<iter);
    int status = specex::cholesky_solve(A,B);
    if(status!=0) SPECEX_ERROR("cholesky solve failed with status " << status);
    SPECEX_INFO("solving done");
    if(fit_dx) ublas::noalias(dxpol.coeff) += ublas::project(B,ublas::range(0,npardx));
    if(fit_flux) ublas::noalias(flux)      += ublas::project(B,ublas::range(npardx,npar));
    int saved_npardx = npardx;
    int saved_npar   = npar;
    
    SPECEX_INFO("compute chi2 iter #" <<iter);

    if(iter==2) 
      compute_segments = true;
    else
      compute_segments = false;
    
    double chi2  = computechi2ab(false,false);
    double dchi2 =  previous_chi2-chi2;
    
    SPECEX_INFO("chi2= " << chi2 << " dchi2= " << dchi2);
    if(dchi2<0) {
      SPECEX_WARNING("neg. dchi2, rewinding ");
      if(fit_dx) ublas::noalias(dxpol.coeff) -= ublas::project(B,ublas::range(0,saved_npardx));
      if(fit_flux) ublas::noalias(flux)      -= ublas::project(B,ublas::range(saved_npardx,saved_npar));
    }

    if(compute_segments) {robustify();}
    
    if(fit_dx) {
      
      for(int fiber = 0; fiber<500; fiber+=100) {
	const specex::Trace& trace = psf->GetTrace(fiber);
	for(double y=500; y<4000;y+=1000) {
	  double wave = trace.W_vs_Y.Value(y);
	  double dx=dxpol.Value(trace.X_vs_W.Value(wave),wave);
	  SPECEX_INFO("delta_x(fiber=" << fiber << ",wave=" << wave << ")=" << dx);
	}
      }
    }
    if(iter>=3 && fabs(previous_chi2-chi2)<10) break;

    previous_chi2 = chi2;

  } // end of loop on iterations
  
  
  // now apply thoses shifts to the PSF

  for(std::map<int,specex::PSF_Params>::iterator bundle_it = psf->ParamsOfBundles.begin(); bundle_it != psf->ParamsOfBundles.end(); bundle_it++) {
    for(int fiber=bundle_it->second.fiber_min; fiber<=bundle_it->second.fiber_max; fiber++) {
      specex::Trace& trace = psf->GetTrace(fiber);
      
      double wmin=trace.X_vs_W.xmin;
      double wmax=trace.X_vs_W.xmax;
      int nwave=(trace.X_vs_W.deg+1)*2;
      harp::vector_double wave(nwave);
      harp::vector_double x(nwave);
      harp::vector_double y(nwave);
      cout << "fiber #" << fiber;
      for(int i=0;i<nwave;i++) {
	wave(i) = wmin+(i*(wmax-wmin))/(nwave-1);
	x(i)    = trace.X_vs_W.Value(wave(i));
	double dx = dxpol.Value(x(i),wave(i));
	cout << " " << dx;
	x(i)   += dx;
	y(i)    = trace.Y_vs_W.Value(wave(i));
      }
      cout << endl;
      trace.X_vs_W.Fit(wave,x,0,false);
      trace.X_vs_Y.Fit(y,x,0,false);
    }
  }

  // write PSF
  if(output_psf_xml_filename !="")
    write_psf_xml(psf, output_psf_xml_filename);
  if(output_psf_fits_filename !="" && psf->Name() == "GaussHermite2PSF") {
    write_psf_fits(psf,output_psf_fits_filename);
  }
  return 0;
}
