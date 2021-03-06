#!/bin/bash

do_master=1
pull_master=1
build_master=1

do_dev=1
loc_dev=1
build_dev=1
inplace_dev=0

devcode='dev'
mstcode='mst'

mstbranch='master'
devbranch='rm_ublas'

# activate desi environment
source /global/common/software/desi/desi_environment.sh
source ${DESICONDA}/etc/profile.d/conda.sh
conda activate

export SPECEXDATA=/global/common/software/desi/cori/desiconda/20200801-1.4.0-spec/code/specex/0.6.7/data

# clear default system specex and desispec
module unload specex
module unload desispec

specex_system=/global/common/software/desi/cori/desiconda/20200801-1.4.0-spec/code/specex/master/lib/python3.8/site-packages/specex-0.6.7.dev617-py3.8-linux-x86_64.egg

desi_compute_psf_args="--input-image /global/cfs/cdirs/desi/spectro/redux/blanc/preproc/20201216/00068217/preproc-b1-00068217.fits --input-psf /global/cfs/cdirs/desi/spectro/redux/blanc/exposures/20201216/00068217/shifted-input-psf-b1-00068217.fits --debug" 

# set CC and CXX to gcc as in harpconfig
#export CC=gcc
#export CXX=gcc
#export CC=icc
#export CXX=icpc

#module unload PrgEnv-gnu
#module load   PrgEnv-intel

if [ $do_master -gt 0 ]; then
    # clean
    rm -f fit*mst*fits mst_version.log
    
    # change current desispec to master
    module load desispec

    # change specex to master
    if [ $pull_master -eq 0 ]; then
	specex_loc=$specex_system
    else
	specex_loc=$PWD/specex-$mstcode/code/lib/python3.8/site-packages/specex-0.7.0.dev637-py3.8-linux-x86_64.egg
    fi
    export PYTHONPATH=$specex_loc:$PYTHONPATH
    if [ $build_master -gt 0 ]; then
	rm -rf specex-$mstcode    
	git clone --single-branch --branch $mstbranch \
	    https://github.com/desihub/specex \
	    specex-$mstcode
	cd specex-$mstcode
	rm -rf CMakeF* CMakeC* build code
	python setup.py install --prefix ./code -v 
	cd ../
    fi
    # do desispec-mst and specex-mst
    code=mst
    echo $specex_loc >> mst_version.log    
    echo `ls -rlt $specex_loc` >> mst_version.log
    echo `which desi_compute_psf` >> mst_version.log
    (time desi_compute_psf $desi_compute_psf_args \
	--output-psf ./fit-psf-b1-00068217-$code.fits) \
	|& tee psf-b1-$code.log
    
    
fi

if [ $do_dev -gt 0 ]; then
    # clean
    rm -f fit*dev*fits dev_version.log

    if [ $build_dev -gt 0 ]; then
	rm -rf specex-$devcode    
	if [ $loc_dev -gt 0 ]; then
	    # get local branch
	    cp -r ../specex-$devcode ./	
	else
	    # get current specex dev branch
	    git clone --single-branch --branch $devbranch \
		https://github.com/desihub/specex \
		specex-$devcode
	fi    

	# compile specex dev 
	cd specex-$devcode; mkdir code
	rm -rf CMakeF* CMakeC* build code
	python setup.py install --prefix ./code -v 
	cd ../

    fi
    
    # change current desispec to master
    module load desispec

    # change specex to dev branch
    if [ $inplace_dev -eq 0 ] ; then
	specex_loc=$PWD/specex-$devcode/code/lib/python3.8/site-packages/specex-0.7.0.dev637-py3.8-linux-x86_64.egg
    else
	specex_loc=/global/common/software/desi/users/malvarez/specex-dev/py/specex:$PYTHONPATH
    fi
    export PYTHONPATH=$specex_loc:$PYTHONPATH
    
    # do specex-dev
    echo $specex_loc >> dev_version.log    
    echo `ls -rlt $specex_loc` >> dev_version.log
    echo `which desi_compute_psf` >> dev_version.log
    code=dev
    (time desi_compute_psf $desi_compute_psf_args \
	 --output-psf ./fit-psf-b1-00068217-$code.fits) \
	|& tee psf-b1-$code.log
    
fi

diff fit-psf-b1-00068217-mst.fits fit-psf-b1-00068217-dev.fits
python compare_psf_orig.py
grep user *log 
