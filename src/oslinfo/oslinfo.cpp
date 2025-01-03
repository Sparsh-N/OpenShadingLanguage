// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

/*
The code in this file is based somewhat on code released by NVIDIA as
part of Gelato (specifically, gsoinfo.cpp).  That code had the following
copyright notice:

   Copyright 2004 NVIDIA Corporation.  All Rights Reserved.

and was distributed under the 3-clause BSD license.
SPDX-License-Identifier: BSD-3-Clause
*/

/// <doc... oslinfo_source>
///
/// Example program using OSLQuery
/// ==============================
///
/// This is the full text of `oslinfo`, a command-line utility that
/// for any shader, will print out its parameters (name, type, default
/// values, and metadata).
///
/// ~~~~
/// <script type="preformatted">
#include <cstring>
#include <iostream>
#include <string>

#include <OpenImageIO/argparse.h>
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/strutil.h>
#include <OpenImageIO/sysutil.h>
#include <OpenImageIO/timer.h>

#include <OSL/oslquery.h>
using namespace OSL;


static std::string searchpath;
static bool verbose  = false;
static bool runstats = false;
static std::string oneparam;
static std::vector<std::string> filenames;



// Print the default values for a parameter built out of integers.
static void
print_default_string_vals(const OSLQuery::Parameter* p, bool verbose)
{
    size_t ne;
    if (p->varlenarray || p->type.arraylen < 0)
        ne = p->sdefault.size();
    else
        ne = p->type.numelements();
    if (verbose) {
        for (size_t a = 0; a < ne; ++a)
            std::cout << "\t\tDefault value: \""
                      << OIIO::Strutil::escape_chars(p->sdefault[a]) << "\"\n";
    } else {
        for (size_t a = 0; a < ne; ++a)
            std::cout << "\"" << OIIO::Strutil::escape_chars(p->sdefault[a])
                      << "\" ";
        std::cout << "\n";
    }
}



// Print the default values for a parameter built out of floats (including
// color, point, etc., or arrays thereof).
static void
print_default_int_vals(const OSLQuery::Parameter* p, bool verbose)
{
    size_t nf = p->type.aggregate;
    size_t ne;
    if (p->varlenarray || p->type.arraylen < 0)
        ne = p->idefault.size() / nf;
    else
        ne = p->type.numelements();
    if (verbose)
        std::cout << "\t\tDefault value:";
    if (p->type.arraylen || nf > 1)
        std::cout << " [";
    for (size_t a = 0; a < ne; ++a) {
        for (size_t f = 0; f < nf; ++f)
            std::cout << ' ' << p->idefault[a * nf + f];
    }
    if (p->type.arraylen || nf > 1)
        std::cout << " ]";
    std::cout << std::endl;
}



// Print the default values for a parameter built out of strings.
static void
print_default_float_vals(const OSLQuery::Parameter* p, bool verbose)
{
    size_t nf = p->type.aggregate;
    size_t ne;
    if (p->varlenarray || p->type.arraylen < 0)
        ne = p->fdefault.size() / nf;
    else
        ne = p->type.numelements();
    if (verbose)
        std::cout << "\t\tDefault value:";
    if (p->type.arraylen || nf > 1)
        std::cout << " [";
    for (size_t a = 0; a < ne; ++a) {
        if (verbose && p->spacename.size() > a && !p->spacename[a].empty())
            std::cout << " \"" << p->spacename[a] << "\"";
        for (size_t f = 0; f < nf; ++f)
            std::cout << ' ' << p->fdefault[a * nf + f];
    }
    if (p->type.arraylen || nf > 1)
        std::cout << " ]";
    std::cout << std::endl;
}



// Print all the metadata for a parameter.
static void
print_metadata(const OSLQuery::Parameter& m)
{
    std::string typestring(m.type.c_str());
    std::cout << "\t\tmetadata: " << typestring << ' ' << m.name << " =";
    for (unsigned int d = 0; d < m.idefault.size(); ++d)
        std::cout << " " << m.idefault[d];
    for (unsigned int d = 0; d < m.fdefault.size(); ++d)
        std::cout << " " << m.fdefault[d];
    for (unsigned int d = 0; d < m.sdefault.size(); ++d)
        std::cout << " \"" << OIIO::Strutil::escape_chars(m.sdefault[d])
                  << "\"";
    std::cout << std::endl;
}



static void
oslinfo(const std::string& name)
{
    OIIO::Timer t(runstats ? OIIO::Timer::StartNow : OIIO::Timer::DontStartNow);
    OSLQuery g;
    g.open(name, searchpath);
    std::string e = g.geterror();
    if (!e.empty()) {
        std::cout << "ERROR opening shader \"" << name << "\" (" << e << ")\n";
        return;
    }
    if (runstats) {
        // display timings in an easy to sort form
        std::cout << t.stop() << " sec for " << name << "\n";
        return;  // don't show anything else, we are just benchmarking
    }

    if (oneparam.empty()) {
        std::cout << g.shadertype() << " \"" << g.shadername() << "\"\n";
        if (verbose) {
            for (unsigned int m = 0; m < g.metadata().size(); ++m)
                print_metadata(g.metadata()[m]);
        }
    }

    for (size_t i = 0; i < g.nparams(); ++i) {
        const OSLQuery::Parameter* p = g.getparam(i);
        if (!p)
            break;
        if (oneparam.size() && oneparam != p->name)
            continue;
        std::string typestring;
        if (p->isstruct)
            typestring = "struct " + p->structname.string();
        else
            typestring = p->type.c_str();
        if (verbose) {
            std::cout << "    \"" << p->name << "\" \""
                      << (p->isoutput ? "output " : "") << typestring << "\"\n";
        } else {
            std::cout << (p->isoutput ? "output " : "") << typestring << ' '
                      << p->name << ' ';
        }
        if (p->isstruct) {
            if (verbose)
                std::cout << "\t\t";
            std::cout << "fields: {";
            for (size_t f = 0; f < p->fields.size(); ++f) {
                if (f)
                    std::cout << ", ";
                std::string fieldname = p->name.string() + '.'
                                        + p->fields[f].string();
                const OSLQuery::Parameter* field = g.getparam(fieldname);
                if (field)
                    std::cout << field->type.c_str() << ' ' << p->fields[f];
                else
                    std::cout << "UNKNOWN";
            }
            std::cout << "}\n";
        } else if (!p->validdefault) {
            if (verbose)
                std::cout << "\t\tUnknown default value\n";
            else
                std::cout << "nodefault\n";
        } else if (p->type.basetype == TypeDesc::STRING)
            print_default_string_vals(p, verbose);
        else if (p->type.basetype == TypeDesc::INT)
            print_default_int_vals(p, verbose);
        else
            print_default_float_vals(p, verbose);
        if (verbose) {
            for (unsigned int i = 0; i < p->metadata.size(); ++i)
                print_metadata(p->metadata[i]);
        }
    }
}



int
main(int argc, char* argv[])
{
    // Globally force classic "C" locale, and turn off all formatting
    // internationalization, for the entire oslinfo application.
    std::locale::global(std::locale::classic());

#ifdef OIIO_HAS_STACKTRACE
    // Helpful for debugging to make sure that any crashes dump a stack
    // trace.
    OIIO::Sysutil::setup_crash_stacktrace("stdout");
#endif

    OIIO::Filesystem::convert_native_arguments(argc, (const char**)argv);

    OIIO::ArgParse ap;
    // clang-format off
    ap.intro("oslinfo -- list parameters of a compiled OSL shader\n" OSL_INTRO_STRING);
    ap.usage("oslinfo [options] file0 [file1 ...]");
    ap.arg("filename")
      .hidden()
      .action([&](cspan<const char*> argv){ filenames.emplace_back(argv[0]); });
    ap.arg("-v", &verbose)
      .help("Verbose output");
    ap.arg("--runstats", &runstats)
      .help("Benchmark shader loading time for queries");
    ap.arg("-p %s:SEARCHPATH", &searchpath)
      .help("Set searchpath for shaders");
    ap.arg("--param %s:NAME", &oneparam)
      .help("Output information about just this parameter");
    // clang-format on

    ap.parse_args(argc, (const char**)argv);
    if (filenames.empty()) {
        ap.print_help();
        return EXIT_SUCCESS;
    }

    for (auto filename : filenames) {
        oslinfo(filename);
    }
    return EXIT_SUCCESS;
}

/// </script>
/// ~~~~
