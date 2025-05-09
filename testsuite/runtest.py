#!/usr/bin/env python

# Copyright Contributors to the Open Shading Language project.
# SPDX-License-Identifier: BSD-3-Clause
# https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

import os
import glob
import sys
import platform
import subprocess
import difflib
import filecmp
import shutil
import re
from itertools import chain

from optparse import OptionParser


def make_relpath (path, start=os.curdir):
    "Wrapper around os.path.relpath which always uses '/' as the separator."
    p = os.path.relpath (path, start)
    return p if sys.platform != "Windows" else p.replace ('\\', '/')


#
# Get standard testsuite test arguments: srcdir exepath
#

srcdir = "."
tmpdir = "."

OSL_BUILD_DIR = os.environ.get("OSL_BUILD_DIR", "..")
OSL_SOURCE_DIR = os.environ.get("OSL_SOURCE_DIR", "../../..")
OSL_TESTSUITE_DIR = os.path.join(OSL_SOURCE_DIR, "testsuite")
OpenImageIO_ROOT = os.environ.get("OpenImageIO_ROOT", None)
OSL_TESTSUITE_ROOT = make_relpath(os.getenv('OSL_TESTSUITE_ROOT',
                                             '../../../testsuite'))
os.environ['OSLHOME'] = os.path.join(OSL_SOURCE_DIR, "src")
OSL_REGRESSION_TEST = os.environ.get("OSL_REGRESSION_TEST", None)


# Options for the command line
parser = OptionParser()
parser.add_option("-p", "--path", help="add to executable path",
                  action="store", type="string", dest="path", default="")
parser.add_option("--devenv-config", help="use a MS Visual Studio configuration",
                  action="store", type="string", dest="devenv_config", default="")
parser.add_option("--solution-path", help="MS Visual Studio solution path",
                  action="store", type="string", dest="solution_path", default="")
(options, args) = parser.parse_args()

if args and len(args) > 0 :
    srcdir = args[0]
    srcdir = os.path.abspath (srcdir) + "/"
    os.chdir (srcdir)
if args and len(args) > 1 :
    OSL_BUILD_DIR = args[1]
OSL_BUILD_DIR = os.path.normpath (OSL_BUILD_DIR)

tmpdir = "."
tmpdir = os.path.abspath (tmpdir)
if platform.system() == 'Windows' :
    redirect = " >> out.txt 2>&1 "
else :
    redirect = " >> out.txt 2>>out.txt "

refdir = "ref/"
mytest = os.path.split(os.path.abspath(os.getcwd()))[-1]
if str(mytest).endswith('.opt') or str(mytest).endswith('.optix') :
    mytest = mytest.split('.')[0]
test_source_dir = os.getenv('OSL_TESTSUITE_SRC',
                            os.path.join(OSL_TESTSUITE_ROOT, mytest))
#test_source_dir = os.path.join(OSL_TESTSUITE_DIR,
#                               os.path.basename(os.path.abspath(srcdir)))

command = ""
outputs = [ "out.txt" ]    # default

# Control image differencing
failureok = 0
failthresh = 0.004
hardfail = 0.01
failpercent = 0.02
failrelative = 0.001
allowfailures = 0
idiff_program = "oiiotool"
idiff_postfilecmd = ""
skip_diff = int(os.environ.get("OSL_TESTSUITE_SKIP_DIFF", "0"))

filter_re = None
cleanup_on_success = False
if int(os.getenv('TESTSUITE_CLEANUP_ON_SUCCESS', '0')) :
    cleanup_on_success = True
oslcargs = "-Wall"

image_extensions = [ ".tif", ".tx", ".exr", ".jpg", ".png", ".rla",
                     ".dpx", ".iff", ".psd" ]

compile_osl_files = True
splitsymbol = ';'

#print ("srcdir = " + srcdir)
#print ("tmpdir = " + tmpdir)
#print ("path = " + path)
#print ("refdir = " + refdir)
print ("test source dir = ", test_source_dir)

if platform.system() == 'Windows' :
    if not os.path.exists("./ref") :
        test_source_ref_dir = os.path.join (test_source_dir, "ref")
        if os.path.exists(test_source_ref_dir) :
            shutil.copytree (test_source_ref_dir, "./ref")
    if os.path.exists (os.path.join (test_source_dir, "src")) and not os.path.exists("./src") :
        shutil.copytree (os.path.join (test_source_dir, "src"), "./src")
    if not os.path.exists(os.path.abspath("data")) :
        shutil.copytree (test_source_dir, os.path.abspath("data"))
else :
    if not os.path.exists("./ref") :
        test_source_ref_dir = os.path.join (test_source_dir, "ref")
        if os.path.exists(test_source_ref_dir) :
            os.symlink (test_source_ref_dir, "./ref")
    if os.path.exists (os.path.join (test_source_dir, "src")) and not os.path.exists("./src") :
        os.symlink (os.path.join (test_source_dir, "src"), "./src")
    if not os.path.exists("./data") :
        os.symlink (test_source_dir, "./data")

pythonbin = sys.executable
#print ("pythonbin = ", pythonbin)

###########################################################################

# Handy functions...

# Compare two text files. Returns 0 if they are equal otherwise returns
# a non-zero value and writes the differences to "diff_file".
# Based on the command-line interface to difflib example from the Python
# documentation
def text_diff (fromfile, tofile, diff_file=None, filter_re=None):
    import time
    try:
        fromdate = time.ctime (os.stat (fromfile).st_mtime)
        todate = time.ctime (os.stat (tofile).st_mtime)
        if filter_re:
          filt = re.compile(filter_re)
          fromlines = [l for l in open (fromfile, 'r').readlines() if filt.match(l) is not None]
          tolines   = [l for l in open (tofile, 'r').readlines() if filt.match(l) is not None]
        else:         
          fromlines = open (fromfile, 'r').readlines()
          tolines   = open (tofile, 'r').readlines()
    except:
        print ("Unexpected error:", sys.exc_info()[0])
        return -1
        
    diff = difflib.unified_diff(fromlines, tolines, fromfile, tofile,
                                fromdate, todate)
    # Diff is a generator, but since we need a way to tell if it is
    # empty we just store all the text in advance
    diff_lines = [l for l in diff]
    if not diff_lines:
        return 0
    if diff_file:
        try:
            open (diff_file, 'w').writelines (diff_lines)
            print ("Diff " + fromfile + " vs " + tofile + " was:\n-------")
#            print (diff)
            print ("".join(diff_lines))
        except:
            print ("Unexpected error:", sys.exc_info()[0])
    return 1



def run_app (app, silent=False, concat=True) :
    command = app
    if not silent :
        command += redirect
    if concat:
        command += " ;\n"
    return command


def osl_app (app):
    apath = os.path.join(OSL_BUILD_DIR, "bin")
    if (platform.system () == 'Windows'):
        # when we use Visual Studio, built applications are stored
        # in the app/$(OutDir)/ directory, e.g., Release or Debug.
        apath = os.path.join(apath, options.devenv_config)
    return os.path.join(apath, app) + " "


def oiio_app (app):
    if OpenImageIO_ROOT :
        return os.path.join (OpenImageIO_ROOT, "bin", app) + " "
    else :
        return app + " "


# Construct a command that will compile the shader file, appending output to
# the file "out.txt".
def oslc (args) :
    return (osl_app("oslc") + oslcargs + " " + args + redirect + " ;\n")


# Construct a command that will run oslinfo, appending output to
# the file "out.txt".
def oslinfo (args) :
    return (osl_app("oslinfo") + args + redirect + " ;\n")


# Construct a command that runs oiiotool, appending console output
# to the file "out.txt".
def oiiotool (args, silent=False) :
    oiiotool_cmd = (oiio_app("oiiotool") + args)
    if not silent :
        oiiotool_cmd += redirect
    oiiotool_cmd += " ;\n"
    return oiiotool_cmd

# Construct a command that runs maketx, appending console output
# to the file "out.txt".
def maketx (args) :
    return (oiio_app("maketx") + args + redirect + " ;\n")

# Construct a command that will compare two images, appending output to
# the file "out.txt".  We allow a small number of pixels to have up to
# 1 LSB (8 bit) error, it's very hard to make different platforms and
# compilers always match to every last floating point bit.
def oiiodiff (fileA, fileB, extraargs="", silent=True, concat=True) :
    threshargs = (" -fail " + str(failthresh)
               + " -failpercent " + str(failpercent)
               + " -hardfail " + str(hardfail)
               + " -warn " + str(2*failthresh)
               + " -warnpercent " + str(failpercent))
    if idiff_program == "idiff" :
        threshargs += (" -failrelative " + str(failrelative)
                     + " -allowfailures " + str(allowfailures))
    command = (oiio_app(idiff_program) + "-a"
               + " " + threshargs
               + " " + extraargs
               + " " + make_relpath(fileA,tmpdir) + idiff_postfilecmd
               + " " + make_relpath(fileB,tmpdir) + idiff_postfilecmd
               + (" --diff" if idiff_program == "oiiotool" else ""))
    if not silent :
        command += redirect
    if concat:
        command += " ;\n"
    return command


# Construct a command that run testshade with the specified arguments,
# appending output to the file "out.txt".
def testshade (args) :
    if os.environ.__contains__('OSL_TESTSHADE_NAME') :
        testshadename = os.environ['OSL_TESTSHADE_NAME'] + " "
    else :
        testshadename = osl_app("testshade")
    return (testshadename + args + redirect + " ;\n")


# Construct a command that run testrender with the specified arguments,
# appending output to the file "out.txt".
def testrender (args) :
    os.environ["optix_log_level"] = "0"
    return (osl_app("testrender") + " " + args + redirect + " ;\n")


# Construct a command that run testoptix with the specified arguments,
# appending output to the file "out.txt".
def testoptix (args) :
    # Disable OptiX logging to prevent messages from the library from
    # appearing in the program output.
    os.environ["optix_log_level"] = "0"
    return (osl_app("testoptix") + " " + args + redirect + " ;\n")


# Run 'command'.  For each file in 'outputs', compare it to the copy
# in 'ref/'.  If all outputs match their reference copies, return 0
# to pass.  If any outputs do not match their references return 1 to
# fail.
def runtest (command, outputs, failureok=0, failthresh=0, failpercent=0, regression=None, filter_re=None) :
#    print ("working dir = " + tmpdir)
    os.chdir (srcdir)
    open ("out.txt", "w").close()    # truncate out.txt

    if options.path != "" :
        sys.path = [options.path] + sys.path

    test_environ = None
    if (platform.system () == 'Windows') and (options.solution_path != "") and \
       (os.path.isdir (options.solution_path)):
        test_environ = os.environ
        libOIIO_path = options.solution_path + "\\libOpenImageIO\\"
        if options.devenv_config != "":
            libOIIO_path = libOIIO_path + '\\' + options.devenv_config
        test_environ["PATH"] = libOIIO_path + ';' + test_environ["PATH"]

    if regression == "BATCHED" :
        if test_environ == None :
            test_environ = os.environ
        test_environ["TESTSHADE_BATCHED"] = "1"

    if regression == "RS_BITCODE" :
        if test_environ == None :
            test_environ = os.environ
        test_environ["TESTSHADE_RS_BITCODE"] = "1"

    print ("command = ", command)
    for sub_command in command.split(splitsymbol):
        sub_command = sub_command.lstrip().rstrip()
        #print ("running = ", sub_command)
        cmdret = subprocess.call (sub_command, shell=True, env=test_environ)
        if cmdret != 0 and failureok == 0 :
            print ("#### Error: this command failed: ", sub_command)
            print ("FAIL")
            print ("Output was:\n--------")
            print (open ("out.txt", 'r').read())
            print ("--------")
            return (1)

    if skip_diff :
        return 0

    err = 0
    if regression == "BASELINE" :
        if not os.path.exists("./baseline") :
            os.mkdir("./baseline") 
        for out in outputs :
            shutil.move(out, "./baseline/"+out) 
    else :
        for out in outputs :
            extension = os.path.splitext(out)[1]
            ok = 0
            # We will first compare out to ref/out, and if that fails, we
            # will compare it to everything else with the same extension in
            # the ref directory.  That allows us to have multiple matching
            # variants for different platforms, etc.
            if regression != None:
                testfiles = ["baseline/"+out]
            else :                     
                testfiles = ["ref/"+out] + glob.glob (os.path.join ("ref", "*"+extension))
            for testfile in (testfiles) :
                # print ("comparing " + out + " to " + testfile)
                if extension == ".tif" or extension == ".exr" :
                    # images -- use idiff
                    cmpcommand = oiiodiff (out, testfile, concat=False, silent=True)
                    # print ("cmpcommand = ", cmpcommand)
                    cmpresult = os.system (cmpcommand)
                elif extension == ".txt" :
                    cmpresult = text_diff (out, testfile, out + ".diff", filter_re=filter_re)
                else :
                    # anything else
                    cmpresult = 0 if filecmp.cmp (out, testfile) else 1
                if cmpresult == 0 :
                    ok = 1
                    break      # we're done
    
            if ok :
                # if extension == ".tif" or extension == ".exr" or extension == ".jpg" or extension == ".png":
                #     # If we got a match for an image, save the idiff results
                #     os.system (oiiodiff (out, testfile, silent=False))
                print ("PASS: ", out, " matches ", testfile)
            else :
                err = 1
                print ("NO MATCH for ", out)
                print ("FAIL ", out)
                if extension == ".txt" :
                    # If we failed to get a match for a text file, print the
                    # file and the diff, for easy debugging.
                    print ("-----" + out + "----->")
                    print (open(out,'r').read() + "<----------")
                    print ("Diff was:\n-------")
                    print (open (out+".diff", 'r').read())
                if extension == ".tif" or extension == ".exr" or extension == ".jpg" or extension == ".png":
                    # If we failed to get a match for an image, send the idiff
                    # results to the console
                    testfile = None
                    if regression != None:
                        testfile = os.path.join ("baseline/", out)
                    else :
                        testfile = os.path.join (refdir, out)
                    os.system (oiiodiff (out, testfile, silent=False))

    return (err)


##########################################################################



#
# Read the individual run.py file for this test, which will define 
# command and outputs.
#
with open(os.path.join(test_source_dir,"run.py")) as f:
    code = compile(f.read(), "run.py", 'exec')
    exec (code)
# if os.path.exists("run.py") :
#     execfile ("run.py")


# Allow a little more slop for slight pixel differences when in DEBUG mode.
if "DEBUG" in os.environ and os.environ["DEBUG"] :
    failthresh *= 2.0
    hardfail *= 2.0
    failpercent *= 2.0

# Allow an environment variable to scale the testsuite image comparison
# thresholds:
if 'OSL_TESTSUITE_THRESH_SCALE' in os.environ :
    thresh_scale = float(os.getenv('OSL_TESTSUITE_THRESH_SCALE', '1.0'))
    failthresh *= thresh_scale
    hardfail *= thresh_scale
    failpercent *= thresh_scale
    failrelative *= thresh_scale
    allowfailures = int(allowfailures * thresh_scale)

# Force out.txt to be in the outputs
##if "out.txt" not in outputs :
##    outputs.append ("out.txt")

# Force any local shaders to compile automatically, prepending the
# compilation onto whatever else the individual run.py file requested.
for filetype in [ "*.osl", "*.h", "*.oslgroup", "*.xml" ] :
    for testfile in glob.glob (os.path.join (test_source_dir, filetype)) :
        shutil.copyfile (testfile, os.path.basename(testfile))
if compile_osl_files :
    compiles = ""
    oslfiles = glob.glob ("*.osl")
    oslfiles.sort() ## sort the shaders to compile so that they always compile in the same order
    for testfile in oslfiles :
        compiles += oslc (testfile)
    command = compiles + command

# If either out.exr or out.tif is in the reference directory but somehow
# is not in the outputs list, put it there anyway!
if (os.path.exists("ref/out.exr") and ("out.exr" not in outputs)) :
    outputs.append ("out.exr")
if (os.path.exists("ref/out.tif") and ("out.tif" not in outputs)) :
    outputs.append ("out.tif")

# Run the test and check the outputs
if OSL_REGRESSION_TEST != None :
    # need to produce baseline images
    ret = runtest (command, outputs, failureok=failureok,
                   failthresh=failthresh, failpercent=failpercent, regression="BASELINE", filter_re=filter_re)
    if ret == 0 :
        # run again comparing against baseline, not ref
        ret = runtest (command, outputs, failureok=failureok,
                       failthresh=failthresh, failpercent=failpercent, regression=OSL_REGRESSION_TEST, filter_re=filter_re)
else :                   
    ret = runtest (command, outputs, failureok=failureok,
                   failthresh=failthresh, failpercent=failpercent, filter_re=filter_re)
    
if ret == 0 and cleanup_on_success :
    for ext in image_extensions + [ ".txt", ".diff", ".oso" ] :
        files = glob.iglob (srcdir + '/*' + ext)
        baselineFiles = glob.iglob (srcdir + '/baseline/*' + ext) 
        for f in chain(files,baselineFiles) :
            os.remove(f)
            #print('REMOVED ', f)

sys.exit (ret)
