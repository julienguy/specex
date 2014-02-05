#include <assert.h>


#include "harp.hpp"

//#include "specex_base_analytic_psf.h"
#include "specex_psf_fitter.h"
#include "specex_brent.h"
#include "specex_legendre.h"
#include "specex_spot.h"
#include "specex_spot_array.h"
#include "specex_linalg.h"
#include "specex_image_data.h"
#include "specex_fits.h"
#include "specex_message.h"
#include "specex_psf_io.h"

//#include <boost/archive/xml_oarchive.hpp>

using namespace std;

void specex::PSF_Fitter::SelectFiberBundle(int bundle) {
  std::map<int,PSF_Params>::iterator it = psf->ParamsOfBundles.find(bundle);
  if(it==psf->ParamsOfBundles.end()) SPECEX_ERROR("no such bundle #" << bundle);
  psf_params = & (it->second);
}

void specex::PSF_Fitter::SetStampLimitsFromPSF(specex::Stamp& stamp, const specex::PSF_p psf, const double &X, const double &Y) {
  stamp = Stamp(*(stamp.parent_image)); // reset
  psf->StampLimits(X,Y,stamp.begin_i,stamp.end_i,stamp.begin_j,stamp.end_j);
  // now check image bounds
  stamp.begin_i = max(0,stamp.begin_i);
  stamp.end_i   = min(stamp.Parent_n_cols(),stamp.end_i);
  stamp.begin_j = max(0,stamp.begin_j);
  stamp.end_j   = min(stamp.Parent_n_rows(),stamp.end_j);
}
 
void specex::PSF_Fitter::SetStampLimitsFromPSF(specex::Stamp& stamp, const specex::PSF_p psf, const double &xc_min, const double &xc_max, const double &yc_min, const double &yc_max) {
  int k,p;
  psf->StampLimits(xc_min,yc_min,stamp.begin_i,k,stamp.begin_j,p);
  psf->StampLimits(xc_max,yc_max,k,stamp.end_i,p,stamp.end_j);
  
  // now check image bounds
  stamp.begin_i = max(0,stamp.begin_i);
  stamp.end_i   = min(stamp.Parent_n_cols(),stamp.end_i);
  stamp.begin_j = max(0,stamp.begin_j);
  stamp.end_j   = min(stamp.Parent_n_rows(),stamp.end_j);
}

struct BrentBox { // handler to data and params needed to compute chi2 in brent routine
  specex::PSF_Fitter& fitter;
  harp::vector_double& delta_P;
  vector<specex::Spot_p>& spots;
  BrentBox(specex::PSF_Fitter& i_fitter,  harp::vector_double& i_delta_P, vector<specex::Spot_p>& i_spots) :
    fitter(i_fitter),
    delta_P(i_delta_P),
    spots(i_spots)
  {}
};



double compute_chi2_for_a_given_step(const double &current_step, BrentBox* bbox) {
  bbox->fitter.Params += current_step*bbox->delta_P;
  double chi2 = bbox->fitter.ParallelizedComputeChi2AB(false);
  bbox->fitter.Params -= current_step*bbox->delta_P; // go back after testing
  if(bbox->fitter.verbose) {
    SPECEX_INFO("brent step=" << current_step << " chi2=" << chi2);
  }
  return chi2;
}



int specex::PSF_Fitter::NPar(int nspots) const {
  int npar = 0; // fluxes
  if(fit_psf) npar += psf->BundleNFitPar(psf_params->bundle_id);
  if(fit_trace) npar += psf->TracesNPar();
  if(fit_flux) npar += nspots;
  if(fit_position) npar += 2*nspots;
#ifdef EXTERNAL_TAIL  
  if(fit_psf_tail) npar += psf->RTailAmplitudePol.coeff.size(); // the tail amplitudes radial 
#ifdef EXTERNAL_Y_TAIL  
  if(fit_psf_tail) npar += 1; // the tail amplitudes along y 
#endif
#endif
#ifdef CONTINUUM
  if(fit_continuum) npar += psf->ContinuumPol.coeff.size();
#endif
  return npar;
}

double specex::PSF_Fitter::ParallelizedComputeChi2AB(bool compute_ab) {
  
  
  int step_j  = (stamp.end_j-stamp.begin_j)/number_of_image_chuncks;
 
  
  //SPECEX_INFO("Begin parallelized ComputeChi2AB j range " << stamp.begin_j << " " << stamp.end_j);
  
  UpdateTmpData(compute_ab);

  
  
  harp::vector_double chi2_of_chunk(number_of_image_chuncks);
  chi2_of_chunk.clear();
  
  int chunk;
  
#pragma omp parallel for 
  for(chunk=0; chunk<number_of_image_chuncks; chunk++) {
    
    int begin_j = stamp.begin_j + chunk*step_j;
    int end_j   = stamp.begin_j + (chunk+1)*step_j;
    if(chunk==number_of_image_chuncks-1) end_j = stamp.end_j;
    
     if(end_j>begin_j) {
      chi2_of_chunk(chunk) = ComputeChi2AB(compute_ab,begin_j,end_j,& A_of_chunk[chunk], & B_of_chunk[chunk],false);
     }else if(compute_ab) {
       A_of_chunk[chunk].clear();
       B_of_chunk[chunk].clear();
     }
  }
  
  
  for(int chunk=1; chunk<number_of_image_chuncks; chunk++) {
    chi2_of_chunk(0) += chi2_of_chunk(chunk);
  }
  if(compute_ab) {
    for(int chunk=1; chunk<number_of_image_chuncks; chunk++) {
      A_of_chunk[0] += A_of_chunk[chunk];
      B_of_chunk[0] += B_of_chunk[chunk];
    }
  }
  //SPECEX_INFO("End of parallelized ComputeChi2AB chi2 = " << chi2_of_chunk(0));
  return chi2_of_chunk(0);
}

void specex::PSF_Fitter::InitTmpData(const vector<specex::Spot_p>& spots) {
  
// load spot_tmp_data 
  spot_tmp_data.clear();
  
  
  for(size_t s=0;s<spots.size();s++) {
    const specex::Spot_p spot=spots[s];
    
    SpotTmpData tmp;
    tmp.flux = spot->flux;
    tmp.x    = psf->Xccd(spot->fiber,spot->wavelength);
    tmp.y    = psf->Yccd(spot->fiber,spot->wavelength);
    tmp.wavelength    = spot->wavelength;
    tmp.fiber         = spot->fiber;
    
    if(fit_psf && tmp.flux<0) tmp.flux =0; // more robust
    
#ifdef EXTERNAL_TAIL
    tmp.frozen_x = tmp.x;
    tmp.frozen_y = tmp.y;
    tmp.frozen_flux = tmp.flux; // that is the only place where we modify frozen_flux
    if(tmp.frozen_flux<0) {
      // SPECEX_WARNING("Setting flux to zero for spot at flux =" << tmp.frozen_flux << " for tail fit")
      tmp.frozen_flux = 0; // for robustness
    }
    tmp.tail_monomials = psf->RTailAmplitudePol.Monomials(tmp.wavelength);
    tmp.tail_amplitude = specex::dot(tmp.tail_monomials,psf->RTailAmplitudePol.coeff);
#endif

    // stamp
    tmp.stamp = Stamp(image);
    SetStampLimitsFromPSF(tmp.stamp,psf,tmp.x,tmp.y);
    tmp.stamp = tmp.stamp.Intersection(stamp);

    // psf parameters
    tmp.psf_all_params = psf->AllLocalParamsXW(tmp.x,tmp.wavelength,psf_params->bundle_id);
    
    spot_tmp_data.push_back(tmp);
  }
}

void specex::PSF_Fitter::UpdateTmpData(bool compute_ab) {


#ifdef EXTERNAL_TAIL
  if(fit_psf_tail) {
    psf->RTailAmplitudePol.coeff =  ublas::project(Params,ublas::range(psf_r_tail_amplitude_index,psf_r_tail_amplitude_index+psf->RTailAmplitudePol.coeff.size())); // update tail amplitude
  }
#endif

#ifdef CONTINUUM
  if(fit_continuum)
    psf->ContinuumPol.coeff = ublas::project(Params,ublas::range(continuum_index,continuum_index+psf->ContinuumPol.coeff.size()));
#endif

  
  // update spot_tmp_data (spots are called several times because we loop on pixels)
  for(size_t s=0;s<spot_tmp_data.size();s++) {
    
    specex::SpotTmpData &tmp = spot_tmp_data[s];
    
    if(fit_flux) tmp.flux = Params(tmp.flux_parameter_index);
    if(fit_trace) {
      tmp.x = specex::dot(ublas::project(Params,ublas::range(tmp.trace_x_parameter_index,tmp.trace_x_parameter_index+tmp.trace_x_monomials.size())),tmp.trace_x_monomials);
      tmp.y = specex::dot(ublas::project(Params,ublas::range(tmp.trace_y_parameter_index,tmp.trace_y_parameter_index+tmp.trace_y_monomials.size())),tmp.trace_y_monomials);
    }
    if(fit_position) {
      tmp.x = Params(tmp.x_parameter_index);
      tmp.y = Params(tmp.y_parameter_index);
    }
    if(fit_psf) {
      //harp::vector_double toto = tmp.psf_all_params;
      tmp.psf_all_params = psf->AllLocalParamsXW_with_FitBundleParams(tmp.x,tmp.wavelength,psf_params->bundle_id,Params);
      /*
      // or , test
      if(npar_fixed_coord>2 && tmp.psf_all_params(2)!=0) {
	
	for(int i=0;i<2;i++) {
	  cout << "TEST all param " << i << " " << tmp.psf_all_params(i) << " " << toto(i) << " " << tmp.psf_all_params(i)-toto(i) << endl;
	}
	int index = 0;
	for(int p=0;p<npar_fixed_coord;p++) {
	  int c_size = psf_params->FitParPolXW[p]->coeff.size();
	  double pval = specex::dot(ublas::project(tmp.psf_monomials,ublas::range(index,index+c_size)),ublas::project(Params,ublas::range(index,index+c_size)));
	  cout << "TEST param " << p << " " << pval << " " << tmp.psf_all_params(p+2) << " diff=" << pval-tmp.psf_all_params(p+2) << endl;
	  index += c_size;
	}
	exit(12);
      }
      */
    }
    if(fit_psf && ( fit_trace || fit_position ) && compute_ab) { // need to update at each step monomials
      int index=0;
      for(int p=0;p<npar_fixed_coord;p++) {
	harp::vector_double legendre_monomials_for_this_psf_parameter = psf_params->FitParPolXW[p]->Monomials(tmp.x,tmp.wavelength);
	size_t m_size = legendre_monomials_for_this_psf_parameter.size();
	ublas::project(tmp.psf_monomials,ublas::range(index,index+m_size))=legendre_monomials_for_this_psf_parameter;
	index += m_size;
      }
    }
    if(fit_psf_tail) {
      tmp.tail_amplitude = specex::dot(psf->RTailAmplitudePol.coeff,tmp.tail_monomials);
    }
    

  }



}

double specex::PSF_Fitter::ComputeChi2AB(bool compute_ab, int input_begin_j, int input_end_j, harp::matrix_double* input_Ap, harp::vector_double* input_Bp, bool update_tmp_data) const 
{

  

  int begin_j = input_begin_j;
  int end_j   = input_end_j;
  harp::matrix_double* Ap = input_Ap;
  harp::vector_double* Bp = input_Bp;
  
  if(begin_j==0) begin_j=stamp.begin_j;
  if(end_j==0) end_j=stamp.end_j;
  if(Ap==0) Ap = & const_cast<specex::PSF_Fitter*>(this)->A_of_chunk[0];
  if(Bp==0) Bp = & const_cast<specex::PSF_Fitter*>(this)->B_of_chunk[0];

  
  if(update_tmp_data) const_cast<specex::PSF_Fitter*>(this)->UpdateTmpData(compute_ab);
  
  harp::vector_double H;
  if(compute_ab) {
    H.resize(nparTot);
  }
  
  double chi2 = 0;
  
  harp::vector_double gradAllPar,gradPos;
  harp::vector_double *gradAllPar_pointer = 0;
  harp::vector_double *gradPos_pointer = 0;
  std::vector<int> indices_of_fitpar_in_allpar;
  
  if(compute_ab) {
    Ap->clear();
    Bp->clear();
    
    if(fit_psf) {
      gradAllPar.resize(psf->LocalNAllPar()); 
      gradAllPar_pointer = &gradAllPar;

      indices_of_fitpar_in_allpar.resize(npar_fixed_coord); 

      const std::vector<Legendre2DPol_p>& AP=psf_params->AllParPolXW;
      const std::vector<Legendre2DPol_p>& FP=psf_params->FitParPolXW;

      // assert(npar_fixed_coord == FP.size()); // ok
      
      size_t fk=0;
      for (size_t ak =0; ak < AP.size(); ++ak) {
	const Legendre2DPol_p FPk = FP[fk];
	const Legendre2DPol_p APk = AP[ak];
	if(APk==FPk) {
	  indices_of_fitpar_in_allpar[fk]=int(ak);
	  fk++; // change free param index for next iteration
	  if(fk>=FP.size()) break;
	}
      }
    
    }// will remain zero if (!fit_psf)
    if(fit_position || fit_trace) {gradPos.resize(2); gradPos_pointer = &gradPos;}
  
    
    
  }
  
  bool use_footprint = (spot_tmp_data.size()>1 && footprint_weight.Nx()>0);
  

#ifdef CONTINUUM
  if(psf_params->fiber_min<psf_params->fiber_min) SPECEX_ERROR("fibers not defined");
  
  harp::vector_double continuum_params;
  harp::vector_double x_of_trace_for_continuum;
  //harp::vector_double w_of_trace_for_continuum;
  double expfact_for_continuum = 0;
  map<int,harp::vector_double> continuum_monomials;
  size_t np_continuum = psf->ContinuumPol.coeff.size();
  bool has_continuum  = fit_continuum;
  if(!has_continuum) for(size_t k=0; k<np_continuum; k++) if(psf->ContinuumPol.coeff[k]!=0) {has_continuum = true; break;}

  if(has_continuum) {
    if(fit_continuum)
      continuum_params = ublas::project(Params,ublas::range(continuum_index,continuum_index+np_continuum));
    else
      continuum_params = psf->ContinuumPol.coeff;
    
    expfact_for_continuum=1./(2*M_PI*square(psf->continuum_sigma_x));
    
    x_of_trace_for_continuum.resize(psf_params->fiber_max-psf_params->fiber_min+1);
  }
#endif

  //BUG DANS CONTINUUM


  for (int j=begin_j; j <end_j; ++j) {
    
#ifdef CONTINUUM
    if(has_continuum) {
      for(int fiber=psf_params->fiber_min;fiber<=psf_params->fiber_max;fiber++) {
	x_of_trace_for_continuum(fiber-psf_params->fiber_min) = psf->GetTrace(fiber).X_vs_Y.Value(j);
	continuum_monomials[fiber]=psf->ContinuumPol.Monomials(psf->GetTrace(fiber).W_vs_Y.Value(j));
      }
    }
#endif  

    for (int i=stamp.begin_i ; i < stamp.end_i; ++i) {
      
      double w = 1;
      if(use_footprint) { 
	w=footprint_weight(i,j); 
      } else{
	w=weight(i,j);
      }
      if (w<=0) continue;

      // w=1; // DEBUG

      double res = double(image(i,j));
      
      if(compute_ab)
	H.clear();


#ifdef CONTINUUM
      if(has_continuum) {
	double continuum_value=0;
	for(int fiber=psf_params->fiber_min;fiber<=psf_params->fiber_max;fiber++) {
	  double continuum_prof = expfact_for_continuum * exp(-0.5*square((i-x_of_trace_for_continuum(fiber-psf_params->fiber_min))/psf->continuum_sigma_x));
	  continuum_value += specex::dot(continuum_params,continuum_monomials[fiber])*continuum_prof;
	  if(compute_ab && fit_continuum) {
	    ublas::project(H,ublas::range(continuum_index,continuum_index+np_continuum)) += continuum_prof*continuum_monomials[fiber];
	  }
	}
	res -= continuum_value;
      }
#endif      
      
      
      int nspots_in_pix = 0;

 
      for(size_t s=0;s<spot_tmp_data.size();s++) { // loop on tmp spots data
	
	const specex::SpotTmpData &tmp = spot_tmp_data[s];
	
	
#ifdef EXTERNAL_TAIL
	
	if(tmp.tail_amplitude!=0 || fit_psf_tail) {
	  double tail_prof = psf->TailProfile(i-tmp.frozen_x,j-tmp.frozen_y);
	  
	  const double& flux_for_tail = tmp.frozen_flux;
	  //const double& flux_for_tail = tmp.flux;
	  
	  res -= flux_for_tail*tmp.tail_amplitude*tail_prof;
	  
	  if (compute_ab) {
	    //if(fit_flux) H(tmp.flux_parameter_index) += tail_val; // only if unfrozen
	    if(fit_psf_tail)
	      ublas::project(H,ublas::range(psf_r_tail_amplitude_index,psf_r_tail_amplitude_index+psf->RTailAmplitudePol.coeff.size())) += (flux_for_tail*tail_prof)*tmp.tail_monomials;
	  }
	}

#endif

	if( ! tmp.stamp.Contains(i,j)) continue;
	
	nspots_in_pix++;
	
	double psfVal =  psf->PSFValueWithParamsXY(tmp.x,tmp.y, i, j, tmp.psf_all_params, gradPos_pointer, gradAllPar_pointer);
	
	//if(psfVal==PSF_NAN_VALUE || isnan(psfVal)) SPECEX_ERROR("PSF value returns NAN");
	
	if(fabs(tmp.flux*psfVal)>1.e20) {
	  SPECEX_ERROR("SEVERE BUG flux,psf,params " << tmp.flux << " " << psfVal << " " << tmp.psf_all_params);
	}

	
	res -= tmp.flux*psfVal;
	
	if (compute_ab) {
	  
	  if(fit_psf) {
	    size_t index = 0;
	    for(int p=0;p<npar_fixed_coord;p++) {
	      size_t m_size = psf_params->FitParPolXW[p]->coeff.size();
	      //blas::axpy(tmp.flux*gradPar[p],ublas::project(tmp.psf_monomials,ublas::range(index,index+m_size)),ublas::project(H,ublas::range(index,index+m_size))); // doesnt compile
	      ublas::project(H,ublas::range(index,index+m_size)) += (tmp.flux*gradAllPar(indices_of_fitpar_in_allpar[p]))*ublas::project(tmp.psf_monomials,ublas::range(index,index+m_size));
	      index += m_size;
	    }
	  }
	  if(fit_trace) {
	    ublas::project(H,ublas::range(tmp.trace_x_parameter_index,tmp.trace_x_parameter_index+tmp.trace_x_monomials.size())) 
	      += (gradPos(0) * tmp.flux)*tmp.trace_x_monomials;
	    ublas::project(H,ublas::range(tmp.trace_y_parameter_index,tmp.trace_y_parameter_index+tmp.trace_y_monomials.size())) 
	      += (gradPos(1) * tmp.flux)*tmp.trace_y_monomials;
	  }
	  
	  if(fit_flux) H(tmp.flux_parameter_index) += psfVal;
	  
	  if(fit_position) {
	    H(tmp.x_parameter_index) += gradPos(0) * tmp.flux;
	    H(tmp.y_parameter_index) += gradPos(1) * tmp.flux;
	  }







	} // end of test compute_ab
      } // end of loop on spots
      
      chi2 += w*res*res;

      /////////////////////////////////////////////////////
      // this is for debugging when developping the code
      //#define TESTING_H_VECTOR
#ifdef TESTING_H_VECTOR

      if(compute_ab && fit_psf && specex::dot(H,H)>0) {
	cout << "DEBUGGING TESTING_H_VECTOR" << endl;
	
	footprint_weight.data.clear();
	footprint_weight(i,j) = w; // only this pixel
	
	int nok = 0;
	for (int j=stamp.begin_j; j <stamp.end_j; ++j) {
	  for (int i=stamp.begin_i ; i < stamp.end_i; ++i) {
	    double w = 1;
	    if(use_footprint) { 
	      w=footprint_weight(i,j); 
	    } else{
	      w=weight(i,j);
	    }
	    if (w<=0) continue;
	    nok++;
	  }
	}
	cout << "nok = " << nok << endl;
	
	double chi2_0 = ComputeChi2AB(false);
	
      
	double eps = 1.e-6;
	for(size_t p=0;p<Params.size();p++) {
	  Params(p) += 0.5*eps;
	  double chi2_plus = ComputeChi2AB(false);
	  Params(p) -= eps;
	  double chi2_minus = ComputeChi2AB(false);
	  
	  double chi2_der_numeric  = (chi2_plus-chi2_minus)/eps;
	  if(chi2_der_numeric) {
	    double chi2_der_analytic = -2*w*res*H(p); 
	  
	    cout << "DEBUGGING chi2 derivative p=" << p << " numeric=" <<  chi2_der_numeric << " analytic=" << chi2_der_analytic << " diff=" << chi2_der_numeric-chi2_der_analytic;
	    if(chi2_der_analytic!=0) cout << " ratio " << chi2_der_numeric/chi2_der_analytic -1; 
	    cout << endl;
	  }
	}
	exit(12);
      }
#endif
      /////////////////////////////////////////////////////


      if (compute_ab) { 	
	specex::syr(w,H,*Ap); //  A += w*h*h.transposed(); this routine takes advantage of the zeros in h, its faster than sparce matrices
	specex::axpy(w*res,H,*Bp); // B += w*res*h;
      }
      
    } // end of loop on pix coord. i
  } // end of loop on pix coord. j
  
  
  // psf priors , only once
  if(fit_psf && !(psf->Priors.empty()) && begin_j == stamp.begin_j) {
    
    int npar = psf->LocalNAllPar();

    for(size_t s=0;s<spot_tmp_data.size();s++) { // loop on tmp spots data
      
      const specex::SpotTmpData &tmp = spot_tmp_data[s];
      
      int index=0;
      int fp=0; // fitted par. index
      for(int ap=0;ap<npar;ap++) {
	
	const specex::Legendre2DPol_p AP=psf_params->AllParPolXW[ap];
	const specex::Legendre2DPol_p FP=psf_params->FitParPolXW[fp];
	
	if(AP==FP) { // only apply prior to fit parameters  
	  
	  size_t c_size = AP->coeff.size();
	  std::map<int,Prior*>::const_iterator it = psf->Priors.find(ap);
       
	  if(it==psf->Priors.end()) {index += c_size; continue;} // no prior for this psf parameter
	

	  SPECEX_INFO("accounting for Prior at fit index " << fp << " and all index " << ap);
	  
	  const Prior* prior = it->second;
	  const double& par = tmp.psf_all_params(ap);
	
	  if (compute_ab) {
	    
	    
	    for (size_t c=0; c<c_size; c++, index++) {
	    
	      const double& monomial_val = tmp.psf_monomials(index);
	      (*Bp)(index)       += monomial_val * prior->hdChi2dx(par);
	      (*Ap)(index,index) += square(monomial_val) * prior->hd2Chi2dx2(par);
	    
	    }
	  }
	  chi2 += prior->Chi2(par);
	  fp++;
	  if(fp>=int(psf_params->FitParPolXW.size())) break;
	}
      }
    }

  }

  //SPECEX_INFO("ComputeChi2AB j range= " << input_begin_j << " " << input_end_j << " chi2= " << chi2);
  return chi2;
}

   
static double sign(const double& a, const double& b) {
  if(b>0) return fabs(a);
  return -fabs(a);
}

void specex::PSF_Fitter::ComputeWeigthImage(vector<specex::Spot_p>& spots, int* npix) {
  /* 
     footprint weight includes :
     - weight set to zero for pixels unaffected by any spot (depends on flux and psf footprint)
     - weight from input image, just to set weight = 0 to bad pixels, we ignore variance that include signal because biasing
     - if include_signal_in_weighg, weight account for Poisson signal to noise.
  */
  
  // definition of fitted region of image
  // ----------------------------------------------------
  double xc_min=1e20;
  double xc_max=-1e20;
  double yc_min=1e20;
  double yc_max=-1e20;
  for(size_t s=0;s<spots.size();++s) {
    const specex::Spot_p spot = spots[s];
    if(spot->xc<xc_min) xc_min=spot->xc;
    if(spot->xc>xc_max) xc_max=spot->xc;
    if(spot->yc<yc_min) yc_min=spot->yc;
    if(spot->yc>yc_max) yc_max=spot->yc;
  }
  SetStampLimitsFromPSF(stamp,psf,xc_min,xc_max,yc_min,yc_max);
  


  if(spots.size()>1) {
    // compute psf footprint
    footprint_weight.resize(weight.Nx(),weight.Ny());
    footprint_weight.data.clear();
    
    
    // if include_signal_in_weight, first store model in footprint_weight

    *npix = 0;
    if(include_signal_in_weight) {
      
      SPECEX_INFO("WEIGHTS: Computing weights");
      SPECEX_INFO("WEIGHTS: readout noise = " << psf->readout_noise);
      SPECEX_INFO("WEIGHTS: gain = " << psf->gain);
      SPECEX_INFO("WEIGHTS: psf error = " << psf_error);
     
      // create a list of stamps
      vector<specex::Stamp> spot_stamps;
      for(size_t s=0;s<spots.size();s++) {
	specex::Spot_p spot = spots[s];
	
	Stamp stamp(image);
	psf->StampLimits(spot->xc,spot->yc,stamp.begin_i,stamp.end_i,stamp.begin_j,stamp.end_j);
	stamp.begin_i = max(0,stamp.begin_i);
	stamp.end_i   = min(stamp.Parent_n_cols(),stamp.end_i);
	stamp.begin_j = max(0,stamp.begin_j);
	stamp.end_j   = min(stamp.Parent_n_rows(),stamp.end_j);
	spot_stamps.push_back(stamp);
      }
      
      bool add_spots_core_signal = true;
      bool zero_weight_for_core  = false;
      bool fill_core_footprint = false;

#ifdef EXTERNAL_TAIL     
      bool has_tails = (psf->RTailAmplitudePol.coeff.size() && psf->RTailAmplitudePol.coeff(0)!=0);
      fill_core_footprint |= ( (! fit_psf_tail) &&  has_tails);
#endif
#ifdef CONTINUUM
      bool has_continuum = (psf->ContinuumPol.coeff.size() && psf->ContinuumPol.coeff(0)!=0);
      fill_core_footprint |= ( (! fit_continuum) &&  has_continuum);
#endif

      image_data core_stamp_footprint;

      if(fill_core_footprint) {
	SPECEX_INFO("WEIGHTS: computing footprint of core of stamps");
	core_stamp_footprint = image_data(image.n_cols(),image.n_rows());
	
	for(size_t s2=0;s2<spots.size();s2++) {
	  const Stamp& spot_stamp = spot_stamps[s2];
	  for (int j=spot_stamp.begin_j; j <spot_stamp.end_j; ++j) {  
	    for (int i=spot_stamp.begin_i ; i < spot_stamp.end_i; ++i) {
	      core_stamp_footprint(i,j)=1;
	    }
	  }
	}
      }

#ifdef EXTERNAL_TAIL

      if( (! fit_psf_tail)  &&  has_tails ) {
	SPECEX_INFO("WEIGHTS: Fill spots stamps model with PSF tails");
      }else if(fit_psf_tail ) {
	SPECEX_INFO("WEIGHTS: Fill all pixels between spots with PSF tail value (to fit tails)");
      }

      if( has_tails || fit_psf_tail) {

	for(size_t s=0;s<spots.size();s++) {
	  specex::Spot_p spot= spots[s];
	  
	  double r_tail_amplitude;
	  if(!fit_psf_tail) {
	    r_tail_amplitude = psf->RTailAmplitudePol.Value(spot->wavelength);
	  } else {
	    r_tail_amplitude = 0.005;
	    if(has_tails) r_tail_amplitude = psf->RTailAmplitudePol.Value(spot->wavelength);
	  }	
	  r_tail_amplitude = max(spot->flux*r_tail_amplitude,1.e-20);
	  
	  for (int j=stamp.begin_j; j <stamp.end_j; ++j) {  // this stamp is a rectangle with all the spots in it
	    
	    int begin_i = max(stamp.begin_i,int(floor(psf->GetTrace(psf_params->fiber_min).X_vs_Y.Value(double(j))+0.5))-psf->hSizeX-1);
	    int end_i   = min(stamp.end_i,int(floor(psf->GetTrace(psf_params->fiber_max).X_vs_Y.Value(double(j))+0.5))+psf->hSizeX+2);
	    
	    for (int i=begin_i ; i < end_i; ++i) {
	      
	      if(weight(i,j)==0) continue; // no need to compute anything
	      if(fill_core_footprint && core_stamp_footprint(i,j)==0) continue; // no need to compute anything
	      footprint_weight(i,j) += r_tail_amplitude*psf->TailProfile(i-spot->xc,j-spot->yc);
	      
	    } 
	  }
	}
      }
      if((fit_psf_tail) && !fit_flux && !fit_psf) { add_spots_core_signal = false; zero_weight_for_core = true;}
      
#endif
      
#ifdef CONTINUUM
      
      // need to add continuum here as well
      if( has_continuum || fit_continuum) {

	SPECEX_INFO("WEIGHTS: Fill continuum");

	for (int j=stamp.begin_j; j <stamp.end_j; ++j) {  
	  
	  
	  int begin_i = max(stamp.begin_i,int(psf->GetTrace(psf_params->fiber_min).X_vs_Y.Value(double(j))-psf->hSizeX));
	  int end_i   = min(stamp.end_i,int(psf->GetTrace(psf_params->fiber_max).X_vs_Y.Value(double(j))+psf->hSizeX+1));
	    

	  for(int fiber=psf_params->fiber_min;fiber<=psf_params->fiber_max;fiber++) {
	    double x = psf->GetTrace(fiber).X_vs_Y.Value(double(j));
	    double w = psf->GetTrace(fiber).W_vs_Y.Value(double(j));
	    double continuum_flux = psf->ContinuumPol.Value(w);
	    
	    if(continuum_flux==0 && fit_continuum) continuum_flux=10; // to assign weights
	    
	    double expfact_for_continuum=continuum_flux/(2*M_PI*square(psf->continuum_sigma_x));
	    
	    if(expfact_for_continuum>0) {
	      
	      for (int i=begin_i ; i <end_i; ++i) {
		
		if(weight(i,j)==0) continue; // no need to compute anything
		if(fill_core_footprint && core_stamp_footprint(i,j)==0) continue; // no need to compute anything
	    
		footprint_weight(i,j) += expfact_for_continuum*exp(-0.5*square((i-x)/psf->continuum_sigma_x));
		
	      } // end of loop on i
	    }
	  } // end of loop on fiber
	} // end of loop on j
      }
      
      if( ( fit_continuum ) &&  !fit_flux && !fit_psf) { add_spots_core_signal = false; zero_weight_for_core = true;}
      
#endif
 
      if(add_spots_core_signal) {

	SPECEX_INFO("WEIGHTS: Fill spots stamps model with PSF core");
	
	for(size_t s=0;s<spots.size();s++) {
	  const specex::Spot_p spot= spots[s];
	  
	  harp::vector_double psfParams = psf->AllLocalParamsXW(spot->xc,spot->wavelength,psf_params->bundle_id);
	  
	  Stamp& spot_stamp = spot_stamps[s];
	  
	  
	  for(int j=spot_stamp.begin_j;j<spot_stamp.end_j;j++) {
	    for(int i=spot_stamp.begin_i;i<spot_stamp.end_i;i++) {
	      if(weight(i,j)==0) continue; // no need to compute anything
	      footprint_weight(i,j) += max(1.e-20,spot->flux*psf->PSFValueWithParamsXY(spot->xc,spot->yc,i,j,psfParams,0,0));
	      
	    }
	  }
	}
      }
      if(zero_weight_for_core) {
	
	SPECEX_INFO("WEIGHTS: Set weight to ZERO at the CORE of spots (to fit tails or continuum)");
	
	for(size_t s=0;s<spots.size();s++) {
	  Stamp& spot_stamp = spot_stamps[s];
	  for(int j=spot_stamp.begin_j;j<spot_stamp.end_j;j++) {
	    for(int i=spot_stamp.begin_i;i<spot_stamp.end_i;i++) {
	      footprint_weight(i,j) = 0; 
	    }
	  }
	}
      }

      // compute variance and weight
      
      SPECEX_INFO("WEIGHTS: Compute weights");
      for (int j=stamp.begin_j; j <stamp.end_j; ++j) {  // this stamp is a rectangle with all the spots in it
	for (int i=stamp.begin_i ; i < stamp.end_i; ++i) {
	  
	  double model_flux=footprint_weight(i,j);
	  if(model_flux==0) continue;
	  
	  double var = square(psf->readout_noise);
	  if(model_flux>0) { // else negative fluctuation
	    var += model_flux/psf->gain;
	  }
	  var += square(psf_error*model_flux);
	  
	  footprint_weight(i,j) = 1./var;
	  (*npix)++;
	}
      }

      
      
    }else{
      // only footprint
      for(size_t s=0;s<spots.size();s++) {
	specex::Spot_p& spot= spots[s];
	Stamp spot_stamp(image);
	SetStampLimitsFromPSF(spot_stamp,psf,spot->xc,spot->yc);
	for(int j=spot_stamp.begin_j;j<spot_stamp.end_j;j++)
	  for(int i=spot_stamp.begin_i;i<spot_stamp.end_i;i++) {
	    footprint_weight(i,j)=weight(i,j);
	  }
      }
    }
    
    // mask out some regions with mis-understood lines
    mask.ApplyMaskToImage(footprint_weight,*psf,0);
    
    (*npix)=0;
    for (int j=stamp.begin_j; j <stamp.end_j; ++j) {  // this stamp is a rectangle with all the spots in it
      for (int i=stamp.begin_i ; i < stamp.end_i; ++i) {
	if(footprint_weight(i,j)>0) (*npix)++;
      }
    }
    
    SPECEX_INFO("WEIGHTS: number of pixels in fit = " << *npix);
    
  } // end of test on spot size

}


bool specex::PSF_Fitter::FitSeveralSpots(vector<specex::Spot_p>& spots, double *chi2, int *n_pixels, int *n_iterations) {
  
  if(fit_trace && fit_position) {
    SPECEX_ERROR("specex::PSF_Fitter::FitSeveralSpots cannot fit traces and spot positions at the same time");
  }

  // chi2 , iterations ...
  // ----------------------------------------------------
  double chi2_memory_slot = 0;
  double *psfChi2 = &chi2_memory_slot;
  if(chi2) psfChi2 = chi2;
  int niter_memory_slot = 0;
  int *niter = &niter_memory_slot;
  if(n_iterations) niter = n_iterations;
  int npixels_memory_slot = 0;
  int *npix = &npixels_memory_slot;
  if(n_pixels) npix = n_pixels;
  *niter=0;
  int maxiter = 100; 
  double oldChi2=1e30;
  if(*psfChi2<=0) *psfChi2 = 1e20;
  
  // ----------------------------------------------------
    
  int npar_psf = 0;
  if(fit_psf) npar_psf = psf->BundleNFitPar(psf_params->bundle_id);
  int npar_trace = 0;
  if(fit_trace) npar_trace = psf->TracesNPar();
  
  npar_fixed_coord = psf_params->FitParPolXW.size();
  npar_varying_coord = psf->BundleNFitPar(psf_params->bundle_id);
  nparTot  = NPar(spots.size());
  
  SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots npar_fixed_coord   = " << npar_fixed_coord);
  SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots npar_varying_coord = " << npar_varying_coord);

  
  if(specex_verbose()) {
    stringstream ss;
    ss << "specex::PSF_Fitter::FitSeveralSpots starting fitting ";
    if(fit_flux) ss << "+flux";
    if(fit_psf) ss << "+psf";
    if(fit_trace) ss << "+trace";
    if(fit_position) ss << "+position";
#ifdef EXTERNAL_TAIL
    if(fit_psf_tail) ss << "+tail";
#endif
    if(fit_psf) ss << " npar_psf=" << npar_psf;
    if(fit_trace) ss << " npar_trace=" << npar_trace;
    specex_info(ss.str());
  }
  
  
  // allocation of parameters, setting Params 
  // and recording some indices
  // ----------------------------------------------------
  Params.resize(nparTot);
  Params.clear();

  map<int,int> tmp_trace_x_parameter;
  map<int,int> tmp_trace_y_parameter;
  int index_of_spot_parameters = 0;
  
  {
    int index=0;
    if(fit_psf) {
      for(size_t p=0;p<psf_params->FitParPolXW.size();p++) {
	const harp::vector_double& coeff=psf_params->FitParPolXW[p]->coeff;
	size_t c_size = coeff.size();
	ublas::project(Params,ublas::range(index,index+c_size)) = coeff;
	index += c_size;
      }
    }
    
    if(fit_trace) {  
      for(std::map<int,specex::Trace>::const_iterator it=psf->FiberTraces.begin(); it!=psf->FiberTraces.end(); ++it) {

	tmp_trace_x_parameter[it->first] = index;

	{
	  const harp::vector_double& coeff = it->second.X_vs_W.coeff;
	  size_t c_size = coeff.size();
	  ublas::project(Params,ublas::range(index,index+c_size)) = coeff;
	  index += c_size;
	}
	
	tmp_trace_y_parameter[it->first] = index;
	
	{
	  const harp::vector_double& coeff = it->second.Y_vs_W.coeff;
	   size_t c_size = coeff.size();
	  ublas::project(Params,ublas::range(index,index+c_size)) = coeff;
	  index += c_size;
	}
      }
    }
#ifdef EXTERNAL_TAIL
    if(fit_psf_tail) {
      psf_r_tail_amplitude_index = index;
      ublas::project(Params,ublas::range(psf_r_tail_amplitude_index,psf_r_tail_amplitude_index+psf->RTailAmplitudePol.coeff.size())) = psf->RTailAmplitudePol.coeff;
      index+=psf->RTailAmplitudePol.coeff.size();
    }
#endif

#ifdef CONTINUUM
    if(fit_continuum) {
      continuum_index = index;
      ublas::project(Params,ublas::range(continuum_index,continuum_index+psf->ContinuumPol.coeff.size())) = psf->ContinuumPol.coeff;
      index += psf->ContinuumPol.coeff.size();
    }
    
#endif
    index_of_spot_parameters = index;
  }
  // ----------------------------------------------------
  
  ComputeWeigthImage(spots,npix);
  
  InitTmpData(spots);
  
  // indexation and Params and monomials for tmp spots 
  {
  int index = index_of_spot_parameters;
  
  for(size_t s=0;s<spot_tmp_data.size();s++) {
    SpotTmpData& tmp = spot_tmp_data[s];
    if(fit_flux) {
      tmp.flux_parameter_index = index; 
      Params(tmp.flux_parameter_index) = tmp.flux;
      index++; 
    }
    if(fit_position) {
      tmp.x_parameter_index = index; 
      Params(tmp.x_parameter_index) = tmp.x;
      index++;
      tmp.y_parameter_index = index; 
      Params(tmp.y_parameter_index) = tmp.y;
      index++;
    }
    if(fit_psf) {
      tmp.psf_monomials.resize(npar_varying_coord);
      // specex::zero(tmp.psf_monomials);
      int index=0;
      for(int p=0;p<npar_fixed_coord;p++) {
	harp::vector_double legendre_monomials_for_this_psf_parameter = psf_params->FitParPolXW[p]->Monomials(tmp.x,tmp.wavelength);
	size_t m_size = legendre_monomials_for_this_psf_parameter.size();
	ublas::project(tmp.psf_monomials,ublas::range(index,index+m_size))=legendre_monomials_for_this_psf_parameter;
	index += m_size;
	}
    }    
    // trace monomials
    if(fit_trace) {
      const specex::Trace& trace = psf->FiberTraces[tmp.fiber];
      tmp.trace_x_monomials = trace.X_vs_W.Monomials(tmp.wavelength);
      tmp.trace_y_monomials = trace.Y_vs_W.Monomials(tmp.wavelength);
      tmp.trace_x_parameter_index = tmp_trace_x_parameter[tmp.fiber];
      tmp.trace_y_parameter_index = tmp_trace_y_parameter[tmp.fiber];
    }
  }
  }
  
  
  if(verbose)
    SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots inc. signal in w=" << include_signal_in_weight << ", npix footprint = " << *npix);


#ifdef EXTERNAL_TAIL
  if(psf->r_tail_profile.n_rows()==0) psf->ComputeTailProfile();
#endif

  ////////////////////////////////////////////////////////////////////////// 
  number_of_image_chuncks = 1;
  char* OMP_NUM_THREADS = getenv("OMP_NUM_THREADS");
  if(OMP_NUM_THREADS) {
    number_of_image_chuncks = atoi(OMP_NUM_THREADS);
    SPECEX_INFO("Using " << number_of_image_chuncks << " image chunks equal to value of OMP_NUM_THREADS");
  }

 
  
  A_of_chunk.clear();
  B_of_chunk.clear();
  for(int i=0;i<number_of_image_chuncks;i++) {
    A_of_chunk.push_back(harp::matrix_double(nparTot, nparTot));
    B_of_chunk.push_back(harp::vector_double(nparTot));
  }
  //////////////////////////////////////////////////////////////////////////
  
  
  while(true) { // minimization loop 
      
    oldChi2 = *psfChi2;
    if(verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots iter=" << *niter << " old chi2=" << oldChi2);
    
    if(verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots filling matrix (n="<< nparTot << ")...");
    *psfChi2 = ParallelizedComputeChi2AB(true);

    oldChi2 = *psfChi2;
    if(verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots chi2 = " << *psfChi2);
    if(verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots solving ...");
    
    harp::matrix_double& A = A_of_chunk[0];
    harp::vector_double& B = B_of_chunk[0];
    

    harp::matrix_double As=A;
    //harp::vector_double Bs=B;
    
    /*
    if(0 && fit_psf && npar_fixed_coord>2) {
      {
	ofstream os("A.xml");
	boost::archive::xml_oarchive xml_oa ( os );
	xml_oa << BOOST_SERIALIZATION_NVP(A);
	os.close();
      }
      {
	ofstream os("B.xml");
	boost::archive::xml_oarchive xml_oa ( os );
	xml_oa << BOOST_SERIALIZATION_NVP(B);
	os.close();
      }
      exit(12);
    }
    */

    
    int status = cholesky_solve(A,B);

    if(verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots solving done");

    if (status != 0) {
      *psfChi2 = 1e30; 
      for(size_t i=0;i<As.size1();i++)
	if(As(i,i)<=0)
	  cout << "DEBUG A(" <<i << "," << i << ")=" << As(i,i) << endl;
      cout << As << endl; //.writeFits("A.fits");
      //Bs.writeASCII("B.dat");
      if(fatal) {
	SPECEX_ERROR("cholesky_solve failed with status " << status);
      } else {
	SPECEX_WARNING("cholesky_solve failed with status " << status);
	psf_params->fit_status = 1;
	psf_params->chi2 = *psfChi2;
	psf_params->nparams = Params.size();
	psf_params->ndata = *npix;
	psf_params->nspots_in_fit = spots.size();
	return false;
      }
    }
    
    
    
    bool linear = false;
    
    if( (fit_flux) && (!fit_position) && (!fit_trace) && (!fit_psf) ) {
      if( verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots linear because only fit flux");
      linear = true;
    }
    if( (fit_psf) && (!fit_position) && (!fit_trace) && (!fit_flux) && psf->IsLinear()) {
      if( verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots linear because only fit psf (linear wrt params)");
      linear = true;
    } 
    if( (fit_psf_tail || fit_continuum) && (!fit_position) && (!fit_trace) && (!fit_flux) && (!fit_psf)) {
      if( verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots linear because only fit tail and continuum");
      linear = true;
    } 

    
    bool use_brent = true;
    
    if(1) {
      if(linear) {
	if( verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots no brent because linear");
	use_brent = false;
      }else{
	// check whether step indeed decreases chi2
	Params += B;
	if( verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots computing chi2 for step=1 ...");
	double chi2_1 = ParallelizedComputeChi2AB(false);
	Params -= B;
	if(chi2_1>oldChi2) {
	  use_brent = true;
	  if( verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots use brent because step 1 increases chi2");
	}else{
	  
	  use_brent = false;
	  *psfChi2 = chi2_1;
	  if( verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots don't brent because chi2 decreases");	  
	}
      }
    }
    if(use_brent) {
      double brent_precision = chi2_precision/10.;
      if(brent_precision>0.1) brent_precision = 0.1;
      
      if(verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots starting brent with precision = " << brent_precision << " ...");
     
      // need to use brent here
      BrentBox bbox(*this,B,spots);
      
      double min_step=0;
      double max_step=2;
      double prefered_step=1;
      int status=0;
      
      // check the chi2 decrement is not good enought with step=1
      double best_chi2=compute_chi2_for_a_given_step(1,&bbox);      
      double best_step = 1;
      
      if(fabs(best_chi2-*psfChi2)>chi2_precision) { // really try brent now
	best_step = brent((AnalyticFunction*)(compute_chi2_for_a_given_step),
			  min_step,prefered_step,max_step,
			  brent_precision,&bbox,best_chi2,status,100);
      }else{
	if(verbose) SPECEX_INFO("brent not needed for required chi2 decrement dchi2=" << (-best_chi2+*psfChi2));
	if(best_chi2>*psfChi2) best_step=0; // check this anyway
      }
      
      if (status != 0) {
	SPECEX_WARNING("specex::PSF_Fitter::FitSeveralSpots brent fit didn't converge" 
		       << " best_step=" << best_step << " best_chi2=" << best_chi2);
	
	if(best_chi2>*psfChi2) {
	  if(fatal) {
	    SPECEX_ERROR("specex::PSF_Fitter::FitSeveralSpots brent doesn't improve things, this is a failure");
	  } else{
	    SPECEX_WARNING("specex::PSF_Fitter::FitSeveralSpots brent doesn't improve things, this is a failure");
	    psf_params->fit_status = 2;
	    psf_params->chi2 = *psfChi2;
	    psf_params->nparams = Params.size();
	    psf_params->ndata = *npix;
	    psf_params->nspots_in_fit = spots.size(); 
	    return false;
	  }
	} else {
	  SPECEX_WARNING("specex::PSF_Fitter::FitSeveralSpots brent improves things so we continue");
	}
      }
      if(verbose) {
	double dchi2 = (-best_chi2+*psfChi2);
	if(dchi2>0 || fabs(dchi2)<brent_precision) {
	  if(verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots successful brent fit with step = " << best_step << " dchi2=" << dchi2
	       << " chi2pdf = " << best_chi2/(*npix-Params.size())
	      );
	}else{
	  SPECEX_ERROR("problem with brent dchi2 = " << dchi2);
	  best_step = 0;
	  best_chi2 = *psfChi2;
	}
      }
      
      Params += best_step*B;
      *psfChi2 = best_chi2;
	
    } else { // didn't use brent
      Params += B;
      // *psfChi2 = ComputeChi2AB(false); // already computed above
      if(verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots dchi2=" << oldChi2-*psfChi2);
      
    }
    
    // some sanity checks flux == nan is really bad
    for (unsigned k=0; k < nparTot; ++k) {
      if (isnan(Params(k))) {
	if(fatal) {
	  SPECEX_ERROR("specex::PSF_Fitter::FitSeveralSpots one parameter read nan");
	} else {
	  SPECEX_WARNING("specex::PSF_Fitter::FitSeveralSpots one parameter read nan");
	}
	*psfChi2 = 1e30;
	psf_params->fit_status = 3;
	psf_params->chi2 = *psfChi2;
	psf_params->nparams = Params.size();
	psf_params->ndata = *npix;
	psf_params->nspots_in_fit = spots.size();
	return false;
      }
    }
    
    (*niter) ++;
    
    // ending tests

    if( (*niter) > maxiter ) {
      SPECEX_WARNING("specex::PSF_Fitter::FitSeveralSpots quit loop because max iterations reached");
      break;
    }
    if( fabs(oldChi2-*psfChi2)<chi2_precision) {
      if( verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots quit loop because dchi2 small");
      break;
    }
    if( linear ) {
      if( verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots quit loop because linear");
      break;
    }
  } // end of minimization loop
    
  // We have to extract the weight matrix of the psf parameters (the first npar of params vector).
  //  This involves "marginalization" over the position and flux of the star. Compute 
  // full covariance matrix (from chi2 second derivatives), extract the psf params sub block,
  // and invert it back to get a weight matrix
  

  
  fitWeight = A_of_chunk[0];
  if (specex::cholesky_invert_after_decomposition(fitWeight) != 0) {
    SPECEX_ERROR("cholesky_invert_after_decomposition failed");
  }
  
  // fitWeight is now a covariance matrix
  // just to avoid confusion :
  harp::matrix_double& fitCovmat = fitWeight;
  
  if(verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots saving fitted params");
  
  int index=0;

  // copy fitted parameters in the right places:
  if (fit_psf) {
    // save params to psf
    
    for(size_t p=0;p<psf_params->FitParPolXW.size();p++) {
      harp::vector_double& coeff=psf_params->FitParPolXW[p]->coeff;
      for(size_t c=0;c<coeff.size();c++,index++)
	coeff(c)=Params(index);
    }
    
    
  }
  if (fit_trace) {
    for(std::map<int,specex::Trace>::iterator it=psf->FiberTraces.begin(); it!=psf->FiberTraces.end(); ++it) {
      harp::vector_double& PX = it->second.X_vs_W.coeff;
      for(size_t k=0;k<PX.size();k++,index++)
	PX(k)=Params(index);
      harp::vector_double& PY = it->second.Y_vs_W.coeff;
      for(size_t k=0;k<PY.size();k++,index++)
	PY(k)=Params(index);
    }
  }
#ifdef EXTERNAL_TAIL
    if(fit_psf_tail) {
      
      psf->RTailAmplitudePol.coeff = ublas::project(Params,ublas::range(psf_r_tail_amplitude_index,psf_r_tail_amplitude_index+psf->RTailAmplitudePol.coeff.size()));
     
      /*
      if(psf->r_tail_amplitude<0) {
	SPECEX_WARNING("specex::PSF_Fitter::FitSeveralSpots negative r tail amplitude " << psf->r_tail_amplitude << " set to 0 ");
	psf->r_tail_amplitude = 0;
      }
      */

#ifdef EXTERNAL_Y_TAIL
      
      psf->y_tail_amplitude = Params(psf_y_tail_amplitude_index);
      if(psf->y_tail_amplitude<0) {
	SPECEX_WARNING("specex::PSF_Fitter::FitSeveralSpots negative y tail amplitude " << psf->y_tail_amplitude << " set to 0 ");
	psf->y_tail_amplitude = 0;
      }

#endif

    }
#endif

#ifdef CONTINUUM
    if(fit_continuum) {
      psf->ContinuumPol.coeff = ublas::project(Params,ublas::range(continuum_index,continuum_index+psf->ContinuumPol.coeff.size()));
    }
#endif


  // save results for spots
  if(verbose) SPECEX_INFO("specex::PSF_Fitter::FitSeveralSpots saving spots fluxes");
  for(size_t s=0;s<spots.size();s++) {
    specex::Spot_p& spot= spots[s];
    specex::SpotTmpData& tmp = spot_tmp_data[s];
    if(fit_flux) {
      spot->flux = Params(tmp.flux_parameter_index); 
      spot->eflux = sqrt(fitCovmat(tmp.flux_parameter_index,tmp.flux_parameter_index));
    }
    if(fit_position) {
      spot->xc = Params(tmp.x_parameter_index);
      spot->yc = Params(tmp.y_parameter_index);
    }
    if(fit_trace) {
      spot->xc = psf->Xccd(spot->fiber,spot->wavelength);
      spot->yc = psf->Yccd(spot->fiber,spot->wavelength);

      if(spot->xc != tmp.x || spot->yc != tmp.y) {
	SPECEX_ERROR("spot coord. " << spot->xc << "," << spot->yc << " tmp " << tmp.x << "," << tmp.y);
      }

    }
  }
  
  // spot->chi2=*psfChi2;
  
  bool ok=(*niter < maxiter);
  if(verbose) {
    
    if(ok) {
      if(specex_verbose()) {
	cout << "INFO specex::PSF_Fitter::FitSeveralSpots successful fit of ";
	if(fit_flux) cout << "+flux";
	if(fit_position) cout << "+pos";
	if(fit_psf) cout << "+psf";
	if(fit_trace) cout << "+trace";
	
#ifdef EXTERNAL_TAIL
	if(fit_psf_tail) cout << "+tail";
#endif
#ifdef CONTINUUM
	if(fit_continuum) cout << "+continuum";
#endif

	cout << " chi2= " << *psfChi2;
	cout << " niter=" << *niter;
	cout << endl;
#ifdef EXTERNAL_TAIL
#ifdef EXTERNAL_Y_TAIL
	if(fit_psf_tail) 
	  SPECEX_INFO("psf tail amplitudes, r: " << Params(psf_r_tail_amplitude_index) << " , y: " << Params(psf_y_tail_amplitude_index));
#else
if(fit_psf_tail) 
	  SPECEX_INFO("psf tail amplitudes, r: " << Params(psf_r_tail_amplitude_index) );
#endif
#endif

#ifdef CONTINUUM
 if(fit_continuum)
   SPECEX_INFO("continuum amplitude, r: " << Params(continuum_index) );
#endif

      }
    }else{
      SPECEX_WARNING("specex::PSF_Fitter::FitSeveralSpots failed because reached max number of iterations");
    }

  }
  
  psf_params->fit_status = 0;
  psf_params->chi2 = *psfChi2;
  psf_params->nparams = Params.size();
  psf_params->ndata = *npix;
  psf_params->nspots_in_fit = spots.size();
  return ok;
};
 
bool specex::PSF_Fitter::FitOneSpot(specex::Spot_p& spot, double *chi2, int *n_iterations) {

  specex::Spot saved_spot = *spot;

  vector<specex::Spot_p> spots; 
  spots.push_back(spot);
  
  
  // int npar_psf = psf->LocalNPar();
  // psf_global_params->clear();
  // for(int p=0;p<npar_psf;p++) {
  //   psf_global_params->push_back(specex::Legendre2DPol(0,0,image.Nx(),0,0,image.Ny()));
  //   (*psf_global_params)[p].coeff(0) = spot->PSFParams(p);
  // }
  
  
  bool ok = FitSeveralSpots(spots,&(spot->chi2),n_iterations);
  
  if(verbose) {
    
    if(ok) {
      cout << "INFO specex::PSF_Fitter::FitOneSpot successful fit of flux";
      if(fit_position) cout << "+pos";
      if(fit_psf) cout << "+psf";
      cout << " chi2= " << spot->chi2;
      cout << " dchi2= " <<  saved_spot.chi2-spot->chi2;
      if(saved_spot.flux>0)
	 cout << " dflux/flux=" << spot->flux/saved_spot.flux-1;
      cout << " dx=" << spot->xc-saved_spot.xc;
      cout << " dy=" << spot->yc-saved_spot.yc;
      if(n_iterations)
      cout << " niter=" << n_iterations;
      cout << endl;
    }else{
      cout << "WARNING specex::PSF_Fitter::FitOneSpot failed because reached max number of iterations" << endl;
    } 
  }
  return ok;
};

/*
void specex::PSF_Fitter::SetAllPSFParams(const harp::vector_double &ParamsToSet) {
  // also, define psf polynomes (should be in fitter)
  psf_params->AllParPolXW.clear();
  psf_params->FitParPolXW.clear();
  for (size_t k =0; k < ParamsToSet.size(); ++k) {
    psf_params->AllParPolXW.push_back(new specex::Legendre2DPol(0,0,image.Nx(),0,0,image.Ny()));
    psf_global_params->push_back(specex::Legendre2DPol(0,0,image.Nx(),0,0,image.Ny()));
    psf_global_params->back().coeff(0)=ParamsToSet(k);
  }
}
*/


bool specex::PSF_Fitter::FitTraces(vector<specex::Spot_p>& spots, int *n_fibers_fitted) {
  
  SPECEX_INFO("specex::PSF_Fitter::FitTraces starting");

  int nok_memory_slot;
  int *nok = &nok_memory_slot;
  if(n_fibers_fitted) nok = n_fibers_fitted;
  
  *nok = 0;
  for(map<int,specex::Trace>::iterator it=psf->FiberTraces.begin();
      it !=psf->FiberTraces.end(); ++it) {
    
    specex::Trace& trace=it->second;
    
    vector<specex::Spot_p> spots_of_fiber;
    for(size_t s=0;s<spots.size();s++) {
      specex::Spot_p& spot = spots[s];
      if(spot->fiber==trace.fiber)
	spots_of_fiber.push_back(spot);
    }
    bool ok = trace.Fit(spots_of_fiber);
    if(ok) { 
      (*nok)++;
      
      for(size_t s=0;s<spots_of_fiber.size();s++) {
	specex::Spot_p& spot = spots_of_fiber[s];
	spot->xc=trace.X_vs_W.Value(spot->wavelength);
	spot->yc=trace.Y_vs_W.Value(spot->wavelength);

      }


    }
  }
  SPECEX_INFO("specex::PSF_Fitter::FitTraces ended nok = " << (*nok) << "/" << psf->FiberTraces.size());
 
  return true;
}

bool specex::PSF_Fitter::FitIndividualSpotFluxes(std::vector<specex::Spot_p>& spots) {
  
  SPECEX_INFO("fitting independently the flux of each spot");
  include_signal_in_weight = false;
  fit_flux                 = true;
  fit_position             = false;
  fit_psf                  = false;
  fit_trace                = false;
  verbose                  = false; // debug
  fatal                    = false; // debug
    
  int nok=0;
  for(size_t s=0;s<spots.size();s++) {
    specex::Spot_p& spot = spots[s];    
    spot->eflux = 0;
    spot->flux = 0;	
    spot->status=-1;
    
    bool ok = FitOneSpot(spot);
    
    if(ok) {
      nok++;
      spot->status=1;
    }else{
      spot->status=0;
      SPECEX_WARNING("fit of flux of spot at x = " << spot->xc << " " << spot->yc << " failed");
    }
    if(int(s)%100==0 && s!=0) SPECEX_INFO("done " << s << "/" << spots.size() << " ...");
  }
  SPECEX_INFO("successful fit of each spot flux for " << nok << "/" << spots.size());
  return true;
}
bool specex::PSF_Fitter::FitIndividualSpotPositions(std::vector<specex::Spot_p>& spots) {
  
  SPECEX_INFO("fitting independently the flux+position of each spot");
  include_signal_in_weight = false;
  fit_flux                 = true;
  fit_position             = true;
  fit_psf                  = false;
  fit_trace                = false;
  verbose                  = false;
  fatal                    = false;
  
  int nok=0;
  for(size_t s=0;s<spots.size();s++) {
    specex::Spot_p& spot = spots[s];    
    spot->initial_xc = spot->xc;
    spot->initial_yc = spot->yc;
    spot->eflux = 0;
    spot->flux = 0;	
    spot->status=-1;
    
    bool ok = FitOneSpot(spot);
    
    if(ok) {
      nok++;
      spot->status=1;
    }else{
      spot->status=0;
    }
    if(int(s)%100==0 && s!=0) SPECEX_INFO("done " << s << "/" << spots.size() << " ...");
  }
  SPECEX_INFO("successful fit of each spot flux+pos for " << nok << "/" << spots.size());
  return true;
}

bool specex::PSF_Fitter::FitEverything(std::vector<specex::Spot_p>& input_spots, bool init_psf) {

  if(input_spots.size()==0) {
    SPECEX_ERROR("in fit_several_spots spotarrays is empty");
  }

  
  
  
  
  verbose = true;
  
  double chi2=1e30;
  int npix = 0;
  int niter = 0;
    
  SPECEX_INFO("starting to fit PSF with " <<  input_spots.size() << " spots");
 
  if(init_psf) {
    
    SPECEX_INFO("init PSF");
    
    FitTraces(input_spots);

    
    // here we need to count number of wavelength and spots per wavelength
    {
      
      double min_wave = 1e12;
      double max_wave = -1e12;
      double min_x = 1e12;
      double max_x = -1e12;      
      for(size_t s=0;s<input_spots.size(); ++s) {
	const specex::Spot_p spot = input_spots[s];
	if(spot->xc < min_x ) min_x = spot->xc;
	if(spot->xc > max_x ) max_x = spot->xc;
	if(spot->wavelength < min_wave ) min_wave = spot->wavelength;
	if(spot->wavelength > max_wave ) max_wave = spot->wavelength;
	
      }
      
      // get max number of spots per fiber
      map<int,int> number_of_spots_per_fiber;
      for(size_t s=0;s<input_spots.size(); ++s) {
	const specex::Spot_p spot = input_spots[s];
	number_of_spots_per_fiber[spot->fiber]++;
      }
      int number_of_fibers = number_of_spots_per_fiber.size();
      int max_number_of_spots_per_fiber = 0;
      for(map<int,int>::const_iterator it=number_of_spots_per_fiber.begin();
	  it != number_of_spots_per_fiber.end(); ++it) {
	if(it->second > max_number_of_spots_per_fiber ) max_number_of_spots_per_fiber = it->second;
      }
      
      SPECEX_INFO("Number of fibers " << number_of_fibers);
      SPECEX_INFO("Max number of spots per fiber " << max_number_of_spots_per_fiber);
     
      
      if(polynomial_degree_along_x>number_of_fibers-1) {
	polynomial_degree_along_x=number_of_fibers-1;
	SPECEX_INFO("Reducing polynomial degree along x to " << polynomial_degree_along_x << " because of number of fibers");
      }
      if(polynomial_degree_along_wave>max_number_of_spots_per_fiber-1) {
	polynomial_degree_along_wave=max_number_of_spots_per_fiber-1;
	SPECEX_INFO("Reducing polynomial degree along wavelength to " << polynomial_degree_along_wave << " because of number of spots");
      }
      
      SPECEX_INFO("Setting PSF polynomial degrees " << polynomial_degree_along_x << " " << polynomial_degree_along_wave);
      
      psf_params->AllParPolXW.clear();
      
      int npar = psf->LocalNAllPar();
      
      harp::vector_double default_params = psf->DefaultParams();
      std::vector<string> param_names = psf->DefaultParamNames();
      SPECEX_INFO("Default PSF params = " <<  default_params);
      
      if(int(default_params.size()) != npar) SPECEX_ERROR("Fatal inconsistency in number of parameters of PSF");
      if(int(param_names.size()) != npar) SPECEX_ERROR("Fatal inconsistency in number of parameters of PSF");
      
      for(int p=0;p<npar;p++) {
	int degx=polynomial_degree_along_x;
	int degw=polynomial_degree_along_wave;
	
	const string& name = param_names[p];
	if(name=="GHSIGX2" || name=="GHSIGY2" || name=="GHSCAL2") {
	  degx=0;
	  degw=1;
	}
	specex::Legendre2DPol_p pol(new specex::Legendre2DPol(degx,min_x,max_x,degw,min_wave,max_wave));
	pol->name = param_names[p];
	pol->coeff.clear();
	pol->coeff(0) = default_params(p);
	//SPECEX_INFO("Coeff for par " << p << " = " << pol->coeff);
	
	psf_params->AllParPolXW.push_back(pol);
	
      }     
    }
  }
  
  

  fatal = true;
  include_signal_in_weight = false;
  chi2_precision = 10;
  bool ok = true;
  
  SPECEX_INFO("Starting FitSeveralSpots FLUX");
  SPECEX_INFO("=======================================");
  fit_flux       = true;
  fit_position   = false;
  fit_psf        = false;
  fit_trace      = false;
#ifdef CONTINUUM
  fit_continuum  = false;
#endif
#ifdef EXTERNAL_TAIL
  fit_psf_tail   = false;
#endif
  
  ok = FitSeveralSpots(input_spots,&chi2,&npix,&niter);
  if(!ok) SPECEX_ERROR("FitSeveralSpots failed for FLUX");
  
  
// selecting spots
  double minimum_signal_to_noise = 30;
  std::vector<specex::Spot_p> selected_spots;
  for(size_t s=0;s<input_spots.size();s++) {
    specex::Spot_p& spot = input_spots[s];
    if(spot->flux>0  && spot->eflux>0 && spot->flux/spot->eflux>minimum_signal_to_noise) {
      spot->status=1;
      selected_spots.push_back(spot);
    }else{
      spot->status=0;
    }
    
  }
  SPECEX_INFO("selected " << selected_spots.size() << " spots out of " << input_spots.size() << " with S/N>" << minimum_signal_to_noise);

  
  // SPECEX_INFO("STOP HERE FOR DEBUG"); return ok;
  
  int saved_psf_hsizex = psf->hSizeX;
  int saved_psf_hsizey = psf->hSizeY;
  
  
  
  {
    SPECEX_INFO("Choose the parameters that participate to the fit : only gaussian terms");
    psf->hSizeX=min(4,psf->hSizeX);
    psf->hSizeY=min(4,psf->hSizeY);
    psf_params->FitParPolXW.clear();
    int npar = psf->LocalNAllPar();
    for(int p=0;p<npar;p++) {
      const string& name = psf->ParamName(p);
      bool ok = false;
      ok |= (name=="GHSIGX" || name=="GHSIGY");
      //ok |= (name=="GHSIGX2");
      //ok |= (name=="GHSIGY2");
      //ok |= (name=="GHSCAL2");
      if(ok)
	psf_params->FitParPolXW.push_back(psf_params->AllParPolXW[p]);
    }
  }
  
  SPECEX_INFO("Starting FitSeveralSpots PSF gaussian terms");
  SPECEX_INFO("===================================================");
  chi2=1e30;
  fit_flux       = false;
  fit_position   = false;
  fit_psf        = true;
  fit_trace      = false;
  ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
  if(!ok) SPECEX_ERROR("FitSeveralSpots failed for PSF");
  
  
  SPECEX_INFO("Starting FitSeveralSpots PSF+FLUX only gaussian terms");
  SPECEX_INFO("========================================================");
  fit_flux       = true;
  fit_position   = false;
  fit_psf        = true;
  fit_trace      = false;
  ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
  if(!ok) SPECEX_ERROR("FitSeveralSpots failed for PSF+FLUX");
  
  if(0) {
  // now add parameters using signal in weights
  {
    SPECEX_INFO("Choose the parameters that participate to the fit : only gaussian terms");
    psf_params->FitParPolXW.clear();
    int npar = psf->LocalNAllPar();
    for(int p=0;p<npar;p++) {
      const string& name = psf->ParamName(p);
      bool ok = false;
      ok |= (name=="GHSIGX" || name=="GHSIGY");
      //ok |= (name=="GHSIGX2");
      //ok |= (name=="GHSIGY2");
      //ok |= (name=="GHSCAL2");
      if(ok)
	psf_params->FitParPolXW.push_back(psf_params->AllParPolXW[p]);
    }
  }
  SPECEX_INFO("Starting FitSeveralSpots PSF+FLUX only gaussian terms (more)");
  SPECEX_INFO("========================================================");
  fit_flux       = true;
  fit_position   = false;
  fit_psf        = true;
  fit_trace      = false;
  ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
  if(!ok) SPECEX_ERROR("FitSeveralSpots failed for PSF+FLUX");
  include_signal_in_weight = false; 
  }
  
  if(scheduled_fit_of_traces) {
    SPECEX_INFO("Starting FitSeveralSpots TRACE");
    SPECEX_INFO("=======================================");
    fit_flux       = false;
    fit_position   = false;
    fit_psf        = false;
    fit_trace      = true;
    ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
    if(!ok) SPECEX_ERROR("FitSeveralSpots failed for TRACE");
    
    SPECEX_INFO("Starting FitSeveralSpots FLUX+TRACE ");
    SPECEX_INFO("=======================================");
    fit_flux       = true;
    fit_position   = false;
    fit_psf        = false;
    fit_trace      = true;
    ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
    if(!ok) SPECEX_ERROR("FitSeveralSpots failed for FLUX+TRACE");
  }
  

  {
    SPECEX_INFO("Choose the parameters that participate to the fit : all but gaussian terms");
    psf->hSizeX=saved_psf_hsizex;
    psf->hSizeY=saved_psf_hsizey;
    psf_params->FitParPolXW.clear();
    int npar = psf->LocalNAllPar();
    for(int p=0;p<npar;p++) {
      const string& name = psf->ParamName(p);
      ok = (name!="GHSIGX" && name!="GHSIGY");
      ok &= (name!="GHSIGX2" && name!="GHSIGY2");
      ok &= (name!="GHSCAL2");

      if(ok)
	psf_params->FitParPolXW.push_back(psf_params->AllParPolXW[p]);
    }
  }

  // lower selection threshold because PSF is now linear in its parameters and hence fit is more robust
  minimum_signal_to_noise = -3; // keep everything except very bizarre neg. flux
  selected_spots.clear();
  for(size_t s=0;s<input_spots.size();s++) {
    specex::Spot_p& spot = input_spots[s];
    if(spot->eflux>0 && spot->flux/spot->eflux>minimum_signal_to_noise) {
      spot->status=1;
      selected_spots.push_back(spot);
    }else{
      spot->status=0;
    }
    
  }
  
  //write_psf_xml(psf,"psf.xml"); exit(12);
  
  SPECEX_INFO("selected " << selected_spots.size() << " spots out of " << input_spots.size() << " with S/N>" << minimum_signal_to_noise);
  
  SPECEX_INFO("Starting FitSeveralSpots PSF");
  SPECEX_INFO("=======================================");
  fit_flux       = false;
  fit_position   = false;
  fit_psf        = true;
  fit_trace      = false;
  ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
  if(!ok) SPECEX_ERROR("FitSeveralSpots failed for PSF+FLUX");
  
  SPECEX_INFO("Starting FitSeveralSpots PSF+FLUX");
  SPECEX_INFO("=======================================");
  fit_flux       = true;
  fit_position   = false;
  fit_psf        = true;
  fit_trace      = false;
  ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
  if(!ok) SPECEX_ERROR("FitSeveralSpots failed for PSF+FLUX");
  
#ifdef CONTINUUM
#ifdef EXTERNAL_TAIL  

  if(scheduled_fit_of_psf_tail && !scheduled_fit_of_continuum) {
    
    SPECEX_INFO("Starting FitSeveralSpots TAIL");
    SPECEX_INFO("=======================================");
    fit_flux       = false;
    fit_position   = false;
    fit_psf        = false;
    fit_trace      = false;
    fit_psf_tail   = true;
    
    include_signal_in_weight = true; 
    
    double saved_psf_error = psf_error;
    psf_error = 1; // to deweight the core of the psf
    
    for(int i=0; i<2; i++) { 
      ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
    }
    fit_psf_tail   = false; // don't fit this anymore
    include_signal_in_weight = false; // restore previous state
    psf_error = saved_psf_error; // restore previous state
  }
  if(scheduled_fit_of_continuum && !scheduled_fit_of_psf_tail) {
    
    SPECEX_INFO("Starting FitSeveralSpots CONTINUUM");
    SPECEX_INFO("=======================================");
    fit_flux       = false;
    fit_position   = false;
    fit_psf        = false;
    fit_trace      = false;
    fit_psf_tail   = false;
    fit_continuum  = true;
    
    include_signal_in_weight = true; 
    
    double saved_psf_error = psf_error;
    psf_error = 1; // to deweight the core of the psf
    
    for(int i=0; i<2; i++) { 
      ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
    }
    fit_continuum   = false; // don't fit this anymore
    include_signal_in_weight = false; // restore previous state
    psf_error = saved_psf_error; // restore previous state
  }
  if(scheduled_fit_of_continuum && scheduled_fit_of_psf_tail) {
    
    SPECEX_INFO("Starting FitSeveralSpots TAIL+CONTINUUM");
    SPECEX_INFO("=======================================");
    fit_flux       = false;
    fit_position   = false;
    fit_psf        = false;
    fit_trace      = false;
    fit_psf_tail   = true;
    fit_continuum  = true;
    
    include_signal_in_weight = true; 
    
    double saved_psf_error = psf_error;
    psf_error = 1; // to deweight the core of the psf
    
    for(int i=0; i<2; i++) { 
      ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
    }
    fit_psf_tail    = false; // don't fit this anymore
    fit_continuum   = false; // don't fit this anymore
    include_signal_in_weight = false; // restore previous state
    psf_error = saved_psf_error; // restore previous state
  }

#endif
#endif

  
  SPECEX_INFO("Starting FitSeveralSpots PSF+FLUX (bis bis)");
  SPECEX_INFO("=======================================");
  fit_flux       = true;
  fit_position   = false;
  fit_psf        = true;
  fit_trace      = false;
  chi2_precision = 10;
  include_signal_in_weight = true;
  ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
  if(!ok) SPECEX_ERROR("FitSeveralSpots failed for PSF+FLUX");

  SPECEX_INFO("Starting FitSeveralSpots FLUX (updated tails)");
  SPECEX_INFO("=======================================");
  fit_flux       = true;
  fit_position   = false;
  fit_psf        = false;
  fit_trace      = false;
  chi2_precision = 10;
  include_signal_in_weight = true;
  ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
  if(!ok) SPECEX_ERROR("FitSeveralSpots failed for FLUX");

  

  if(scheduled_fit_of_continuum && scheduled_fit_of_psf_tail) {
    
    SPECEX_INFO("Starting FitSeveralSpots TAIL+CONTINUUM (bis)");
    SPECEX_INFO("=============================================");
    fit_flux       = false;
    fit_position   = false;
    fit_psf        = false;
    fit_trace      = false;
    fit_psf_tail   = true;
    fit_continuum  = true;
    
    include_signal_in_weight = true; 
    
    double saved_psf_error = psf_error;
    psf_error = 1; // to deweight the core of the psf
    ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
    
    fit_psf_tail    = false; // don't fit this anymore
    fit_continuum   = false; // don't fit this anymore
    include_signal_in_weight = false; // restore previous state
    psf_error = saved_psf_error; // restore previous state
  }
  
  SPECEX_INFO("Starting FitSeveralSpots PSF+FLUX (bis bis bis)");
  SPECEX_INFO("=======================================");
  fit_flux       = true;
  fit_position   = false;
  fit_psf        = true;
  fit_trace      = false;
  chi2_precision = 1;
  include_signal_in_weight = true;
  ok = FitSeveralSpots(selected_spots,&chi2,&npix,&niter);
  if(!ok) SPECEX_ERROR("FitSeveralSpots failed for PSF+FLUX");
  
    
  SPECEX_INFO("Compute final chi2");
  SPECEX_INFO("=======================================");
  
  
  fit_flux       = false;
  fit_position   = false;
  fit_psf        = false;
  fit_trace      = false;
#ifdef CONTINUUM
  fit_continuum  = false;
#endif
#ifdef EXTERNAL_TAIL
  fit_psf_tail   = false;
#endif  
  include_signal_in_weight = true;
  ComputeWeigthImage(selected_spots,&npix);  
  InitTmpData(selected_spots);
  psf_params->chi2 = ParallelizedComputeChi2AB(false);
  return ok;
}
