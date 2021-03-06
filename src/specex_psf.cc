#include <iomanip>
#include <cmath>
#include <string>

#include "specex_psf.h"
//#include "specex_base_analytic_psf.h"
#include "specex_message.h"
#include "specex_unbst.h"

using namespace std;

/* weights and abcissa for gauss integrations (borrowed from DAOPHOT),
   explained in Numerical recipes */

static double Dx[4][4] ={{0.00000000,  0.0,        0.0       , 0.0       },
			 {-0.28867513,  0.28867513, 0.0       , 0.0       },
			 {-0.38729833,  0.00000000, 0.38729833, 0.0       },
			 {-0.43056816, -0.16999052, 0.16999052, 0.43056816}};
static double Wt[4][4]= {{1.00000000,  0.0       , 0.0       , 0.0       },
			 {0.50000000,  0.50000000, 0.0       , 0.0       },
			 {0.27777778,  0.44444444, 0.27777778, 0.0       },
			 {0.17392742,  0.32607258, 0.32607258, 0.17392742}};
 

// number of points (per coordinate) to integrate over a pixel.
#define NPT 4

//#define INTEGRATING_TAIL

double specex::PSF::PixValue(const double &Xc, const double &Yc,
				     const double &XPix, const double &YPix,
				     const unbls::vector_double &Params,
				     unbls::vector_double *PosDer,
				     unbls::vector_double *ParamDer) const
{
  double xPixCenter = floor(XPix+0.5);
  double yPixCenter = floor(YPix+0.5);
  
  unbls::vector_double tmpPosDer;
  if(PosDer) {
    tmpPosDer.resize(2);
    unbls::zero(tmpPosDer);
  }
  unbls::vector_double tmpParamDer;
  int npar=0;
  if(ParamDer) {
    npar = ParamDer->size();
    tmpParamDer.resize(npar);
    unbls::zero(tmpParamDer);
   }
  
  double val = 0;
  for (int ix=0; ix<NPT; ++ix)
    {
      double x = xPixCenter+Dx[NPT-1][ix] - Xc;
      double wx = Wt[NPT-1][ix];
      for (int iy=0; iy<NPT; ++iy)
	{
	  double y = yPixCenter+Dx[NPT-1][iy] -Yc;
	  double weight = wx*Wt[NPT-1][iy];
	  double prof = Profile(x,y,Params,PosDer,ParamDer);
	  if(prof ==  PSF_NAN_VALUE) return  PSF_NAN_VALUE;
#ifdef EXTERNAL_TAIL
#ifdef INTEGRATING_TAIL
	  double tail_prof = TailProfile(x,y,Params,true); // TRY INTEGRATING TAIL
	  prof += Params(psf_tail_amplitude_index)*tail_prof;
	  if(ParamDer) (*ParamDer)(psf_tail_amplitude_index) = tail_prof;
#endif
#endif
	  
	  
	  val += weight*prof;
	  
	  if (PosDer) 
	    //tmpPosDer += weight*(*PosDer);
	    unbst::subadd(*PosDer,tmpPosDer,0,weight);
	  
	  if (ParamDer)
	    //tmpParamDer += weight*(*ParamDer);
	    unbst::subadd(*ParamDer,tmpParamDer,0,weight);	  
	  
	}
    }
  
  if (PosDer)
    *PosDer = tmpPosDer;
  
  if (ParamDer)
    *ParamDer = tmpParamDer; 
  
  
  return val;
}



specex::PSF::PSF() {
  name = "unknown";
  hSizeX = hSizeY = 12;

#ifdef EXTERNAL_TAIL
  r_tail_profile_must_be_computed = true;
  psf_tail_amplitude_index = -1;
#endif
  
  gain=1;
  readout_noise=1;
  psf_error=0.0;
  arc_exposure_id=0;
  mjd=0;
  plate_id=0;
  ccd_image_n_cols=0;
  ccd_image_n_rows=0;
  camera_id="unknown";
}

#ifdef EXTERNAL_TAIL
#define NX_TAIL_PROFILE 1000
#define NY_TAIL_PROFILE 8000
#define TAIL_OVERSAMPLING 2.

double specex::PSF::TailProfileValue(const double& dx, const double &dy) const {
  double r2 = square(dx*r_tail_x_scale)+square(dy*r_tail_y_scale);
  return r2/(r2_tail_core_size+r2)*pow(r2_tail_core_size+r2,-r_tail_power_law_index/2.);
}

void specex::PSF::ComputeTailProfile(const unbls::vector_double &Params) {
  if(r_tail_profile_must_be_computed == false) {
    SPECEX_WARNING("calling specex::PSF::ComputeTailProfile when r_tail_profile_must_be_computed =false");
    return;
  }

  SPECEX_INFO("specex::PSF::ComputeTailProfile ...");
  
  r_tail_profile.resize(NX_TAIL_PROFILE,NY_TAIL_PROFILE); // hardcoded
  
  if(! HasParam("TAILCORE"))
    SPECEX_ERROR("in PSF::ComputeTailProfile, missing param TAILCORE, need to allocate them, for instance with PSF::AllocateDefaultParams()");
  

  r2_tail_core_size = square(Params[ParamIndex("TAILCORE")]);
  r_tail_x_scale   = Params[ParamIndex("TAILXSCA")];
  r_tail_y_scale   = Params[ParamIndex("TAILYSCA")];
  r_tail_power_law_index = Params[ParamIndex("TAILINDE")];
  
  
  for(int j=0;j<NY_TAIL_PROFILE;j++) {
    for(int i=0;i<NX_TAIL_PROFILE;i++) {
      r_tail_profile(i,j) = TailProfileValue(i/TAIL_OVERSAMPLING,j/TAIL_OVERSAMPLING);
    }
  }

  // store index of PSF tail amplitude
  psf_tail_amplitude_index = ParamIndex("TAILAMP");

  r_tail_profile_must_be_computed = false;
  SPECEX_INFO("specex::PSF::ComputeTailProfile done");
   
  
}

double specex::PSF::TailProfile(const double& dx, const double &dy, const unbls::vector_double &Params, bool full_calculation) const {
  
  if(r_tail_profile_must_be_computed) {
#pragma omp critical
    {
      const_cast<specex::PSF*>(this)->ComputeTailProfile(Params);
    }
  }
  if(full_calculation) return TailProfileValue(dx,dy);
  
  double dxo = fabs(dx*TAIL_OVERSAMPLING);
  double dyo = fabs(dy*TAIL_OVERSAMPLING);
  int di = int(dxo);
  int dj = int(dyo);
  if(di>=NX_TAIL_PROFILE-1 || dj>=NY_TAIL_PROFILE-1) return 0.;
  return r_tail_profile(di,dj); // faster
  return (
	  (di+1-dxo)*(dj+1-dyo)*r_tail_profile(di,dj)
	  +(dxo-di)*(dj+1-dyo)*r_tail_profile(di+1,dj)
	  +(di+1-dxo)*(dyo-dj)*r_tail_profile(di,dj+1)
	  +(dxo-di)*(dyo-dj)*r_tail_profile(di+1,dj+1)
	  );
}

#endif

int specex::PSF::BundleNFitPar(int bundle_id) const {
  std::map<int,PSF_Params>::const_iterator it = ParamsOfBundles.find(bundle_id);
  if(it==ParamsOfBundles.end()) SPECEX_ERROR("no such bundle #" << bundle_id);
  const std::vector<Pol_p>& P=it->second.FitParPolXW;
  int n=0;
  for(size_t p=0;p<P.size();p++)
    n += P[p]->coeff.size();
  return n;
}

specex::PSF::~PSF() {
}

const std::string& specex::PSF::ParamName(int p) const {
  if(!ParamsOfBundles.size()) SPECEX_ERROR("Requiring name of param when no params of bundles in PSF");
  if(p>=int(ParamsOfBundles.begin()->second.AllParPolXW.size())) SPECEX_ERROR("Requiring name of param at index " << p << " when only " << ParamsOfBundles.begin()->second.AllParPolXW.size() << " PSF params");
  return ParamsOfBundles.begin()->second.AllParPolXW[p]->name;
}
int specex::PSF::ParamIndex(const std::string& name) const {
  if(!ParamsOfBundles.size()) return -1;
  for(size_t p=0;p< ParamsOfBundles.begin()->second.AllParPolXW.size();p++) {
    if( ParamsOfBundles.begin()->second.AllParPolXW[p]->name == name) return int(p);
  }
  return -1;
};
bool specex::PSF::HasParam(const std::string& name) const { 
  return (ParamIndex(name)>=0);
}
/*
void specex::PSF::AllocateDefaultParams() {
  unbls::vector_double params = DefaultParams();
  std::vector<std::string> param_names = DefaultParamNames();
  
  PSF_Params pars;
  ParamsOfBundles[0] = pars;
  for(size_t p = 0; p<params.size(); p++) {
    Pol_p pol(new Pol(0,0,1,0,0,1));
    pol->Fill();
    pol->name = param_names[p];
    ParamsOfBundles[0].AllParPolXW.push_back(pol);
  }
}
*/

void specex::PSF::StampLimits(const double &X, const double &Y,
			    int &BeginI, int &EndI,
			    int &BeginJ, int &EndJ) const {
  
  int iPix = int(floor(X+0.5));
  int jPix = int(floor(Y+0.5));
  BeginI = iPix-hSizeX;
  BeginJ = jPix-hSizeY;
  EndI = iPix+hSizeX+1;
  EndJ = jPix+hSizeY+1; 
}

const specex::Trace& specex::PSF::GetTrace(int fiber) const {
  std::map<int,specex::Trace>::const_iterator it = FiberTraces.find(fiber);
  if(it == FiberTraces.end()) SPECEX_ERROR("No trace for fiber " << fiber);
  return it->second;
}

specex::Trace& specex::PSF::GetTrace(int fiber)  {
  std::map<int,specex::Trace>::iterator it = FiberTraces.find(fiber);
  if(it == FiberTraces.end()) SPECEX_ERROR("No trace for fiber " << fiber);
  return it->second;
}

void specex::PSF::LoadXYPol() {
#pragma omp critical
  {
  XPol.clear();
  YPol.clear();
  for(std::map<int,specex::Trace>::iterator it = FiberTraces.begin(); it != FiberTraces.end(); ++it) {
    XPol[it->first] = &(it->second.X_vs_W);
    YPol[it->first] = &(it->second.Y_vs_W);
  }
  }
}

double specex::PSF::Xccd(int fiber, const double& wave) const {
  std::map<int,specex::Legendre1DPol*>::const_iterator it = XPol.find(fiber);
  if(it==XPol.end()) {
    const_cast<specex::PSF*>(this)->LoadXYPol();
    it = XPol.find(fiber);
    if(it == XPol.end()) SPECEX_ERROR("No trace for fiber " << fiber);
  }
  return it->second->Value(wave);
}

double specex::PSF::Yccd(int fiber, const double& wave) const {
  std::map<int,specex::Legendre1DPol*>::const_iterator it = YPol.find(fiber);
  if(it==YPol.end()) {
    const_cast<specex::PSF*>(this)->LoadXYPol();
    it = YPol.find(fiber);
    if(it == YPol.end()) SPECEX_ERROR("No trace for fiber " << fiber);
  }
  return it->second->Value(wave);
}  


//! Access to the current PSF, with user provided Params.
double specex::PSF::PSFValueWithParamsXY(const double &Xc, const double &Yc, 
					 const int IPix, const int JPix,
					 const unbls::vector_double &Params,
					 unbls::vector_double *PosDer, unbls::vector_double *ParamDer,
					 bool with_core, bool with_tail) const {
  
  if(PosDer) unbls::zero(*PosDer);
  if(ParamDer) unbls::zero(*ParamDer);

  double val = 0;
  if(with_core) val += PixValue(Xc,Yc,IPix, JPix, Params, PosDer, ParamDer); 

#ifdef EXTERNAL_TAIL
#ifdef INTEGRATING_TAIL
  if(with_tail && !with_core) {
    double prof = TailProfile(IPix-Xc,JPix-Yc, Params, with_core);
    if(ParamDer) (*ParamDer)(psf_tail_amplitude_index) = prof;
    val += Params(psf_tail_amplitude_index)*prof;
  }
#else
  if(with_tail) {
    double prof = TailProfile(IPix-Xc,JPix-Yc, Params, with_core);
    if(ParamDer) (*ParamDer)[psf_tail_amplitude_index] = prof;
    val += Params[psf_tail_amplitude_index]*prof;
  }
#endif
#endif
  return val;
}

//! Access to the current PSF, with user provided Params.
double specex::PSF::PSFValueWithParamsFW(const int fiber, const double &wave,
				     const int IPix, const int JPix,
				     const unbls::vector_double &Params,
					 unbls::vector_double *PosDer, unbls::vector_double *ParamDer,
					 bool with_core, bool with_tail) const {
  
  double X=Xccd(fiber,wave);
  double Y=Yccd(fiber,wave);
  return PSFValueWithParamsFW(X,Y,IPix,JPix,Params,PosDer,ParamDer,with_core,with_tail);
}

int specex::PSF::GetBundleOfFiber(int fiber) const {
  // find bundle for this fiber
  int bundle=0;
  bool found=false;
  for(std::map<int,specex::PSF_Params>::const_iterator it=ParamsOfBundles.begin() ; it != ParamsOfBundles.end(); ++it) {
    
    if (fiber>=it->second.fiber_min && fiber<=it->second.fiber_max) {
      SPECEX_DEBUG("Found bundle of fiber #" << fiber << " : " << it->first);
      bundle=it->first;
      found=true;
      break;
    }
  }
  if(!found) {
    SPECEX_ERROR("Did not find any bundle for fiber #" << fiber);
  }
  return bundle;
}

unbls::vector_double specex::PSF::AllLocalParamsXW(const double &X, const double &wave, int bundle_id) const {
  
  std::map<int,PSF_Params>::const_iterator it = ParamsOfBundles.find(bundle_id);
  if(it==ParamsOfBundles.end()) SPECEX_ERROR("no such bundle #" << bundle_id);
  const std::vector<Pol_p>& P=it->second.AllParPolXW;
  
  unbls::vector_double params(P.size());
  for (size_t k =0; k < P.size(); ++k)
    params[k] = P[k]->Value(X,wave);
  
  return params;
}

unbls::vector_double specex::PSF::AllLocalParamsFW(const int fiber, const double &wave, int bundle_id) const {
  if(bundle_id<0) { // not given
    bundle_id = GetBundleOfFiber(fiber);
  }

  double X=Xccd(fiber,wave); 
  return AllLocalParamsXW(X,wave,bundle_id);
}

unbls::vector_double specex::PSF::AllLocalParamsXW_with_FitBundleParams(const double &X, const double &wave, int bundle_id, const unbls::vector_double& ForThesePSFParams) const {
  
  
  unbls::vector_double params(LocalNAllPar());
  
  std::map<int,PSF_Params>::const_iterator it = ParamsOfBundles.find(bundle_id);
  if(it==ParamsOfBundles.end()) SPECEX_ERROR("no such bundle #" << bundle_id);
  const std::vector<Pol_p>& AP=it->second.AllParPolXW;
  const std::vector<Pol_p>& FP=it->second.FitParPolXW;
  
  // whe need to find which param is fixed and which is not
  size_t fk=0;
  int index=0;
  for (size_t ak =0; ak < AP.size(); ++ak) { // loop on all params
    const Pol_p APk = AP[ak];
    
    if((fk<FP.size()) && (APk==FP[fk])) { // this is a fit param because the addresses are the same

      size_t c_size = APk->coeff.size();

      params[ak]=specex::dot(ForThesePSFParams,index,index+c_size,APk->Monomials(X,wave));
      
      //SPECEX_INFO("DEBUG all param " << ak << " and fit = " << fk << " are the same, param val =  " << params(ak));

      index += c_size;
      // change free param index for next iteration
      fk++;
    }else{ // this not a free param
      params[ak]=APk->Value(X,wave);
      //SPECEX_INFO("DEBUG all param " << ak << " is not it fit, param val = " << params(ak));
    }
  }
  return params;
}

bool specex::PSF::IsLinear() const {
  if(Name() == "GAUSSHERMITE") return true;
  return false;
}


