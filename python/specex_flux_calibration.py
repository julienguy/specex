#!/usr/bin/env python


# based on spflux_v5.pro
# we do not refit the kurucz parameters or redshift
# we simply load the correct kurucz model with correct redshift and apply
# magnitude correction and reddenning

import pyfits,sys,json,pylab,string,numpy,os,scipy,scipy.sparse,scipy.linalg
from scipy.sparse.linalg import spsolve
from math import *




# convert air to vacuum, this is IDL routine airtovac for instance :
# http://idlastro.gsfc.nasa.gov/ftp/pro/astro/airtovac.pro
def convert_air_to_vacuum(air_wave) :
    # idl code :
    # for iter=0, 1 do begin
    # sigma2 = (1d4/double(wave_vac[g]) )^2.     ;Convert to wavenumber squared
    # ; Compute conversion factor
    # fact = 1.D +  5.792105D-2/(238.0185D0 - sigma2) + $
    #                        1.67917D-3/( 57.362D0 - sigma2)
    # wave_vac[g] = wave_air[g]*fact              ;Convert Wavelength
    # endfor
        
    sigma2 = (1e4/air_wave)**2
    fact = 1. +  5.792105e-2/(238.0185 - sigma2) +  1.67917e-3/( 57.362 - sigma2)
    vacuum_wave = air_wave*fact

    # comparison with http://www.sdss.org/dr7/products/spectra/vacwavelength.html
    # where : AIR = VAC / (1.0 + 2.735182E-4 + 131.4182 / VAC^2 + 2.76249E8 / VAC^4)
    # air_wave=numpy.array([4861.363,4958.911,5006.843,6548.05,6562.801,6583.45,6716.44,6730.82])
    # expected_vacuum_wave=numpy.array([4862.721,4960.295,5008.239,6549.86,6564.614,6585.27,6718.29,6732.68])
    # test ok
    return vacuum_wave

# from idlutils/pro/dust/ext_ccm.pro
def ccm_dust_extinction(wave,Rv=3.1) :
    
    xx = 10000./wave
    indices_LO  = numpy.where(xx>8.0)[0]                                                 # No data, lambda < 1250 Ang
    indices_FUV = numpy.intersect1d(numpy.where(xx>5.9)[0],numpy.where(xx<=8.0)[0])      # UV + FUV
    indices_NUV = numpy.intersect1d(numpy.where(xx>3.3)[0],numpy.where(xx<=5.9)[0])      # UV + FUV
    indices_OPT = numpy.intersect1d(numpy.where(xx>1.1)[0],numpy.where(xx<=3.3)[0])      # Optical/NIR
    indices_IR  = numpy.intersect1d(numpy.where(xx>0.3)[0],numpy.where(xx<=1.1)[0])      # IR
    indices_HI  = numpy.where(xx<=0.3)[0]                                                # No data, lambda > 33,333 Ang
    
    extinction = numpy.zeros(wave.shape)
    #tmp 
    yy    = numpy.zeros(wave.shape)
    afac  = numpy.zeros(wave.shape)
    bfac  = numpy.zeros(wave.shape)
    
    extinction[indices_LO]=5.0
    
    afac[indices_FUV] = 1.752 - 0.316*xx[indices_FUV] - 0.104 / ( (xx[indices_FUV]-4.67)**2 + 0.341 ) - 0.04473*(xx[indices_FUV]-5.9)**2 - 0.009779*(xx[indices_FUV]-5.9)**3
    bfac[indices_FUV] = -3.090 + 1.825*xx[indices_FUV] + 1.206 / ( (xx[indices_FUV]-4.62)**2 + 0.263 ) + 0.2130*(xx[indices_FUV]-5.9)**2 + 0.1207*(xx[indices_FUV]-5.9)**3
    
    afac[indices_NUV] = 1.752 - 0.316*xx[indices_NUV] - 0.104 / ( (xx[indices_NUV]-4.67)**2 + 0.341 ) 
    bfac[indices_NUV] = -3.090 + 1.825*xx[indices_NUV] + 1.206 / ( (xx[indices_NUV]-4.62)**2 + 0.263 ) 
    
    yy[indices_OPT] = xx[indices_OPT] - 1.82
    afac[indices_OPT] = 1.0 + 0.17699*yy[indices_OPT] \
        - 0.50447*yy[indices_OPT]**2 - 0.02427*yy[indices_OPT]**3 \
        + 0.72085*yy[indices_OPT]**4 + 0.01979*yy[indices_OPT]**5 \
        - 0.77530*yy[indices_OPT]**6 + 0.32999*yy[indices_OPT]**7
    bfac[indices_OPT] = 1.41338*yy[indices_OPT] \
        + 2.28305*yy[indices_OPT]**2 + 1.07233*yy[indices_OPT]**3 \
        - 5.38434*yy[indices_OPT]**4 - 0.62251*yy[indices_OPT]**5 \
        + 5.30260*yy[indices_OPT]**6 - 2.09002*yy[indices_OPT]**7
    
    yy[indices_IR] = xx[indices_IR]**1.61
    afac[indices_IR] = 0.574*yy[indices_IR]
    bfac[indices_IR] = -0.527*yy[indices_IR]
    
    yy[indices_HI] = xx[indices_HI]**1.61
    afac[indices_HI] = 0.574*yy[indices_HI]
    bfac[indices_HI] = -0.527*yy[indices_HI]
    
    extinction = afac + bfac / Rv
    
    return extinction

# from idlutils/pro/dust/ext_odonnell.pro
def odonnell_dust_extinction(wave,Rv=3.1) :
    xx = 10000./wave
    
    indices_optical = numpy.intersect1d(numpy.where(xx>=1.1)[0],numpy.where(xx<=3.3)[0])
    indices_other   = numpy.union1d(numpy.where(xx<1.1)[0],numpy.where(xx>3.3)[0])

    extinction = numpy.zeros(wave.shape)
    # tmp
    yy    = numpy.zeros(wave.shape)
    afac  = numpy.zeros(wave.shape)
    bfac  = numpy.zeros(wave.shape)
    
    yy[indices_optical] = xx[indices_optical] - 1.82

    afac[indices_optical] = 1.0 + 0.104*yy[indices_optical] \
        - 0.609*yy[indices_optical]**2 + 0.701*yy[indices_optical]**3 \
        + 1.137*yy[indices_optical]**4 - 1.718*yy[indices_optical]**5 \
        - 0.827*yy[indices_optical]**6 + 1.647*yy[indices_optical]**7 \
        - 0.505*yy[indices_optical]**8

    bfac[indices_optical] = 1.952*yy[indices_optical] \
        + 2.908*yy[indices_optical]**2 - 3.989*yy[indices_optical]**3 \
        - 7.985*yy[indices_optical]**4 + 11.102*yy[indices_optical]**5 \
        + 5.491*yy[indices_optical]**6 - 10.805*yy[indices_optical]**7 \
        + 3.347*yy[indices_optical]**8
    
    extinction[indices_optical] = afac[indices_optical] + bfac[indices_optical] / Rv
    
    if len(indices_other)>0 :
        extinction[indices_other] = ccm_dust_extinction(wave[indices_other],Rv)
    
    return extinction
    
    

# returns a spectrum based on model parameters for each fiber
# spec_parameters is a dictionnary of of dictionnaries (primary key is the fiber)
# target_wave is an array of wavelength 
def specex_read_kurucz(modelfilename, spec_parameters, target_wave=None) :

    print "reading kurucz spectra in",modelfilename
    
    hdulist=pyfits.open(modelfilename)

    table=hdulist[1].data
    data=hdulist[0].data
    n_model_spec=data.shape[0]
    n_model_wave=data.shape[1]
    
    spec_indices=[]

    # find corresponding models
    spec=0
    for fiber in spec_parameters :
        #print fiber,spec_parameters[fiber]
        tmp=numpy.where(table.field("MODEL")==spec_parameters[fiber]["MODEL"])[0]
        if len(tmp)!=1 :
            print "error in specex_read_kurucz cannot find model",par["MODEL"]
            sys.exit(12)
        spec_indices.append(tmp[0])
        spec+=1
        
    print "kurucz spectra used = ",spec_indices
    
   
    
    # load wavelength array
    # wave number (CRPIX1-1) is given by CRVAL1, wave step is CD1_1 (-1 because in fits standard first pix index=1)
    crpix1=hdulist[0].header['CRPIX1']
    crval1=hdulist[0].header['CRVAL1']
    cd1_1=hdulist[0].header['CD1_1']
    model_wave_step   = cd1_1
    model_wave_offset = (crval1-cd1_1*(crpix1-1))
    model_air_wave=model_wave_step*numpy.arange(n_model_wave) + model_wave_offset
    
    # convert air to vacuum
    model_wave_at_redshift0 = convert_air_to_vacuum(model_air_wave)

    # need to duplicate here for each spectrum
    model_wave=numpy.zeros((len(spec_indices),len(model_wave_at_redshift0)))
    
    # apply redshift (norm doesn't matter, fixed afterwards)
    spec=0
    for fiber in spec_parameters :
        redshift=spec_parameters[fiber]["Z"]
        model_wave[spec] = (1.+redshift)*model_wave_at_redshift0
        spec+=1
    
    # model fluxes 
    model_flux = data[spec_indices]
    
    # convert erg/s/cm2/A to photons/s/cm2/A 
    # number of photons = energy/(h*nu) = energy * wl/(2*pi*hbar*c)
    # 2*pi* hbar*c = 2* pi * 197 eV nm = 6.28318*197.326*1.60218e-12*10 = 1.986438e-8 = 1/5.034135e7 ergs.A    
    # (but we don't care of the norm. anyway here)
    model_flux *= 5.034135e7*model_wave
    
    
    if target_wave == None :
        
        # no rebinning requested, we simply keep data in generous range 2500,12000A
        #waveindexmin=n_model_wave
        #waveindexmax=0
        #for spec in range(model_wave.shape[0]) :
        #    waveindexmin=min(waveindexmin,numpy.where(model_wave[spec]>2500)[0][0]-1)
            #waveindexmax=max(waveindexmax,numpy.where(model_wave[spec]>12000)[0][0])
        #model_wave = model_wave[:,waveindexmin:]
        #model_flux = model_flux[:,waveindexmin:]
        
        hdulist.close() 
        return model_wave,model_flux
    
    n_target_wave=len(target_wave)
    
    # keep only data in wave range of interest before rebinning 
    
    wavemin=target_wave[0]-(target_wave[1]-target_wave[0])
    wavemax=target_wave[-1]+(target_wave[-1]-target_wave[-2])
    waveindexmin=n_model_wave
    waveindexmax=0
    for spec in range(model_wave.shape[0]) :
        waveindexmin=min(waveindexmin,numpy.where(model_wave[spec]>wavemin)[0][0]-1)
        waveindexmax=max(waveindexmax,numpy.where(model_wave[spec]>wavemax)[0][0]+2)
    model_wave = model_wave[:,waveindexmin:waveindexmax]
    model_flux = model_flux[:,waveindexmin:waveindexmax]
    
    #print model_flux.shape
    #print model_wave[0,:20]
    #print model_flux[0,:20]    
    #print target_wave[0],target_wave[-1]
    #print wavemin,wavemax
    #print waveindexmin,waveindexmax,len(model_wave),len(model_air_wave)
    
    
    
    # now integrate models in bins

    # compute bounds of bins (ok for any monotonous binning)    
    wavebinbounds=numpy.zeros((target_wave.shape[0]+1))
    wavebinbounds[1:-1]=(target_wave[:-1]+target_wave[1:])/2
    wavebinbounds[0]=target_wave[0]-0.5*(target_wave[1]-target_wave[0])
    wavebinbounds[-1]=target_wave[-1]+0.5*(target_wave[-1]-target_wave[-2])
    
    # mean in bins, quite slow (few seconds), there must be a faster version of this
    flux=numpy.zeros((len(spec_indices),n_target_wave))
    for s in range(model_wave.shape[0]) :
        i1=numpy.where(model_wave[s]>wavebinbounds[0])[0][0]
        for w in range(n_target_wave):
            i2=numpy.where(model_wave[s]>wavebinbounds[w+1])[0][0]
            flux[s,w]=numpy.mean(model_flux[s,i1:i2])
            #print w,i1,i2,wavebinbounds[w],wavebinbounds[w+1]
            i1=i2

    hdulist.close()
    return target_wave,flux

# read spFluxcalib fits image
def get_kurucz_parameters(spfluxcalibfilename,starfibers) :
    print "reading kurucz parameters in",spfluxcalibfilename
    hdulist=pyfits.open(spfluxcalibfilename)
    table=hdulist[2].data
    columns=hdulist[2].columns
    modelparams={}
    for fiber in starfibers :
        row=numpy.where(table.field("FIBERID")==fiber+1)[0][0]
        #print fiber,row
        dico={}
        for k in columns.names :
            dico[k]=table.field(k)[row]
        modelparams[fiber]=dico
    
    hdulist.close()
    return modelparams

def read_spframe(spframefilename) :
    print "reading spFrame table in",spframefilename
    hdulist=pyfits.open(spframefilename)
    table=hdulist[5].data
    columns=hdulist[5].columns
    keys=columns.names
    dico={}
    fibers=table.field("FIBERID")
    for fiberid in fibers :
        myfiberid=fiberid-1
        row=numpy.where(table.field("FIBERID")==fiberid)[0][0]
        entry={}
        for k in keys :
            entry[k]=table.field(k)[row]
        dico[myfiberid]=entry
    
    hdulist.close()
    return dico

# input fluxes have to be in (photons/cm2/s/A), NOT (ergs/cm2/s/A)
def compute_ab_mags(wave,flux,filter_filenames) :
    
    nbands=len(filter_filenames)
    nwave=wave.shape[0]

    #print nbands,wave.shape,flux.shape
    
    filter_transmissions=numpy.zeros((nbands,nwave))
    
    band=0
    for filename in filter_filenames :
        print "reading",filename

        fwave=[]
        ftrans=[]

        file=open(filename)
        for line in file.readlines() :
            if line[0]=="#" :
                continue
            line=line.strip()
            if len(line)==0 :
                continue
            tmp=line.split(" ")
            vals=[]
            for t in tmp :
                if len(t) == 0 :
                    continue
                vals.append(string.atof(t))
            fwave.append(vals[0])
            ftrans.append(vals[1])
        file.close()
        
        fwave=numpy.array(fwave)
        ftrans=numpy.array(ftrans)
        print band,len(wave),len(fwave),len(ftrans)
        filter_transmissions[band]=numpy.interp(wave,fwave,ftrans)
        band+=1
    
    #print filter_transmissions
    
    # compute integrated flux of input spectrum in electrons
    # input spectrum has to be propto photons/cm2/s/A, not ergs
    # because unit of transmission is electron/photon (it includes a quantum efficiency in electron/photon, the rest of the terms are adimentional)
    integrated_flux=numpy.zeros((nbands))
    for b in range(nbands) :
        integrated_flux[b]=numpy.dot(filter_transmissions[b],flux)
    
        
    # compute AB flux in photons/s/cm2/A , NOT ergs, 
    # we have to be precise here because it defines the relation between spec. and imaging magnitudes
    # Fukugita 1996 :
    # Mag_AB = -2.5 log10( flux (ergs/cm2/s/Hz) ) -48.60
    # We want :
    # Mag_AB = -2.5 log10( flux (photons/cm2/s/A) / AB_ref )
    # so,
    # AB_ref = (photons/ergs) c/lambda**2 (in Hz/A) * 10**(-48.6/2.5)
    #        = (photons/ergs) 2.99792458 * 10**(18-48.6/2.5) / lambda**2 (with lambda in A, c from PDG, no harm to be precise)
    # (photons/energy) = lambda/(2*pi*hbar*c) , with 2*pi* hbar*c = 2* pi * 197.326 eV nm = 6.28318*197.326*1.60218e-12*10 = 1.986438e-8 = 1/5.034135e7 ergs.A 
    # AB_ref = (5.034135e7*lambda) * 2.99792458 * 10**(18-48.6/2.5) / lambda**2 (with lambda in A)
    # AB_ref = 5.479558e6/lambda photons/cm2/s/A , with lambda in A

    ab_spectrum = 5.479558e6/wave

    # compute integrated AB flux
    integrated_ab_flux=numpy.zeros((nbands))
    for b in range(nbands) :
        integrated_ab_flux[b]=numpy.dot(filter_transmissions[b],ab_spectrum)
    
    # compute magnitudes
    ab_mags=numpy.zeros((nbands))
    for b in range(nbands) :
        if integrated_flux[b]<=0 :
            ab_mags[b]=99.
        else :
            ab_mags[b]=-2.5*log10(integrated_flux[b]/integrated_ab_flux[b])

    return ab_mags


# main

# test
if False :
    wave=1000.+10*numpy.arange(2000)
    #xx=10000/wave
    dust_extinction1=odonnell_dust_extinction(wave,3.1)
    dust_extinction2=ccm_dust_extinction(wave,3.1)
    pylab.plot(wave,dust_extinction1)
    pylab.plot(wave,dust_extinction2)
    pylab.show()
    sys.exit(12)



if len(sys.argv)<6 :
    print sys.argv[0],"inspec.fits plPlugMapM.par spFluxCalib.fits spFrame.fits outspec.fits"
    sys.exit(12);

infilename=sys.argv[1]
plplgmap=sys.argv[2]
spfluxcalibfilename=sys.argv[3]
spframefilename=sys.argv[4]
outfilename=sys.argv[5]
specexdatadir=""

try :
    specexdatadir=os.environ["SPECEXDATA"]
except :
    print "error:  you need to set the environment variable SPECEXDATA because this code needs various data"
    sys.exit(12)

modelfilename=specexdatadir+"/kurucz_stds_raw_v5.fits"
if not os.path.isfile(modelfilename) :
    print "error, cannot find",modelfilename
    sys.exit(12);
imaging_filters=[]
for band in ["u","g","r","i","z"] :
    filename="%s/sdss_jun2001_%s_atm.dat"%(specexdatadir,band)
    if not os.path.isfile(filename) :
        print "error, cannot find",filename
        sys.exit(12);
    imaging_filters.append(filename)

# get spectrograph id, hardcoded for now, will be read in fits
specid=1

# find  fibers with std stars
starfibers=[]
file=open(plplgmap)
for line in file.readlines() :
    if line.find("PLUGMAPOBJ") != 0 : 
        continue
    
    
    vals=string.split(line," ")
    holetype=vals[8]
    if holetype != "OBJECT" :
        continue
    objType=vals[21]
    if objType != "SPECTROPHOTO_STD" :
        continue
    spectrographId=string.atoi(vals[24])
    if spectrographId != specid :
        continue
    fiberId=string.atoi(vals[25])
    #print line
    #print objType,spectrographId,fiberId
    myfiberid=fiberId-1
    if specid==2 :
        myfiberid-=500
    starfibers.append(myfiberid)
file.close()

print "std stars fibers (now starting at 0)=",starfibers

hdulist=pyfits.open(infilename)
spectra=hdulist[0].data
invar=hdulist[1].data
wave=hdulist[2].data
Rdata=hdulist[3].data

#print "DEBUG: do only few stars"
#starfibers = starfibers[0:10]

nstarfibers=len(starfibers)
nfibers=Rdata.shape[0]
d=Rdata.shape[1]/2
nwave=Rdata.shape[2]
offsets = range(d,-d-1,-1)

# read kurucz model parameters for each fiber
model_params   = get_kurucz_parameters(spfluxcalibfilename,starfibers)


# read ebv and calibrated ab magnitudes, I know in advance there are 5 bands
ebv = numpy.zeros((nstarfibers))
measured_ab_mags = numpy.zeros((nstarfibers,5))

spframe_params = read_spframe(spframefilename)
for spec in range(nstarfibers) :
    fiber=starfibers[spec]
    ebv[spec]=spframe_params[fiber]["SFD_EBV"]
    measured_ab_mags[spec]=spframe_params[fiber]["MAG"]
    #calibfluxes=spframe_params[fiber]["CALIBFLUX"]
    #for b in range(5) :
    #    measured_ab_mags[spec,b]=22.5-2.5*log10(calibfluxes[b]) # THIS IS GUESSED FROM spflux_v5.pro !!!!!!!!!!!!!!!
    

# load model spectra at native resolution to compute ab mags and compare with measured_ab_mags,
unbinned_wave,unbinned_model_fluxes=specex_read_kurucz(modelfilename,model_params,None)

print unbinned_wave.shape

# apply extinction to models
for spec in range(nstarfibers) :
    unbinned_model_fluxes[spec] *= 10.**(-(3.1 * ebv[spec] / 2.5) * odonnell_dust_extinction(unbinned_wave[spec],3.1) )

# compute model magnitudes to apply scaling
model_ab_mags = numpy.zeros((nstarfibers,5))
for spec in range(nstarfibers) :
    model_ab_mags[spec] = compute_ab_mags(unbinned_wave[spec],unbinned_model_fluxes[spec],imaging_filters)

print model_ab_mags

# compute scale to apply to models
# as in spflux_v5, used only band 2(of id) = 'r' = band 2(of python) ????? to define the norm 
scale_to_apply_to_models = numpy.zeros((nstarfibers))
scale_to_apply_to_models[:] = 10**(-0.4*(measured_ab_mags[:,2]-model_ab_mags[:,2]))
 
print scale_to_apply_to_models

# now compute model flux at the wavelength of our data
# fluxes are redshifted and resampled but not dust extinguised nor flux calibrated
unused,model_fluxes=specex_read_kurucz(modelfilename,model_params,wave)

# apply extinction to models
for spec in range(nstarfibers) :
    model_fluxes[spec] *= 10.**(-(3.1 * ebv[spec] / 2.5) * odonnell_dust_extinction(wave,3.1) )

# apply pre-computed scale to models
for spec in range(nstarfibers) :
    model_fluxes[spec] *=  scale_to_apply_to_models[spec]

# models are in electrons/s/cm2/A
# data are electrons/A
# so the ratio data/model is in cm2*s

if False :
    if os.path.isfile("models.fits") :
        os.unlink("models.fits")
    pyfits.HDUList([pyfits.PrimaryHDU(model_fluxes),pyfits.ImageHDU(numpy.zeros(model_fluxes.shape)),pyfits.ImageHDU(wave)]).writeto("models.fits")

# now we dump the ratio data/model to see how it goes, this is only for monitoring

# first convolve models to data resolution
convolved_model_fluxes=numpy.zeros(model_fluxes.shape)
for spec in range(nstarfibers) :
    fiber=starfibers[spec]
    R=scipy.sparse.dia_matrix((Rdata[fiber],offsets),(nwave,nwave))
    convolved_model_fluxes[spec]=numpy.dot(R.todense(),model_fluxes[spec])

# compute the ratio
calib_flux=numpy.zeros(model_fluxes.shape)
calib_flux_invar=numpy.zeros(convolved_model_fluxes.shape)
for spec in range(nstarfibers) :
    fiber=starfibers[spec]
    calib_flux[spec]=spectra[fiber]/convolved_model_fluxes[spec]
    calib_flux_invar[spec]=invar[fiber]*convolved_model_fluxes[spec]*convolved_model_fluxes[spec]

# apply a scaling with exposure time and telescope aperture (approximatly) to compute throughput
telescope_aperture=3.1415*( ((2.5e2)/2)**2 - ((1e2)/2)**2 ) # cm2
exptime=900. #s
calib_flux /= (telescope_aperture*exptime) # s cm2 -> 1
calib_flux_invar *= (telescope_aperture*exptime)**2

if True :
    ofilename="throughput.fits"
    if os.path.isfile(ofilename) :
        os.unlink(ofilename)
    pyfits.HDUList([pyfits.PrimaryHDU(calib_flux),pyfits.ImageHDU(calib_flux_invar),pyfits.ImageHDU(wave)]).writeto(ofilename)


sys.exit(0)

