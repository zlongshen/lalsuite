/*
*  Copyright (C) 2007 Duncan Brown, Stephen Fairhurst
*
*  This program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with with program; see the file COPYING. If not, write to the
*  Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston,
*  MA  02111-1307  USA
*/
#include <lal/LALDatatypes.h>
#include <lal/Calibration.h>
#include <lal/LALCache.h>
#ifndef _FRAMECALIBRATION_H
#define _FRAMECALIBRATION_H

#if defined(__cplusplus)
extern "C" {
#elif 0
}       /* so that editors will match preceding brace */
#endif

/**
 * \defgroup FrameCalibration_h Header FrameCalibration.h
 * \ingroup lalframe_general
 *
 * \author Brown, D. A.
 *
 * \brief High-level routines for exctracting calibration data from frames.
 *
 * ### Synopsis ###
 *
 * \code
 * #include <lal/FrameCalibration.h>
 * \endcode
 *
 * This module contains code used to extract calibration information contained
 * in frame files, and to construct a response (or transfer) function.
 * Provides a high level interface for building a transfer or response
 * functions from raw calibration data provided by the calibration team.
 *
 * This is supposed to provide a high-level interface for search authors to obtain a
 * response function in the desired form.
 *
 * ### Prototypes ###
 *
 * The routine LALExtractFrameResponse() extracts the necessary
 * calibration information from the frames. The frames used to construct
 * the calibration are located using the specified LAL frame cache. The
 * function constructs a response function (as a frequency series) from this
 * information.  The fourth argument is a pointer to a CalibrationUpdateParams
 * structure.  If the ifo field is non-NULL then this
 * string specifies the detector (H1, H2, L1, etc.) for which the calibration
 * is required.  If the duration field is non-zero then the
 * calibration will be averaged over the specified duration.  If the
 * duration is set to zero, then the first calibration at or before
 * the start time is used.  The alpha and alphabeta fields of the structure are
 * required to be zero.  Certain fields of the output should be set before
 * this routine is called.  In particular:
 *
 * The epoch field of the frequency series should be set to the correct
 * epoch so that the routine can generate a response function tailored to that
 * time (accounting for calibration drifts).
 *
 * The units of the response function should be set to be either
 * strain-per-count (for a response function) or count-per-strain (for a
 * transfer function); the routine will then return either the response
 * function or its inverse depending on the specified units.  Furthermore, the
 * power-of-ten field of the units is examined to scale the response function
 * accordingly.
 *
 * The data vector should be allocated to the required length and the
 * frequency step size should be set to the required value so that the routine
 * can interpolate the response function to the required frequencies.
 *
 * The format of the LAL frame cache must contain frames of the following
 * types.
 *
 * It must contain an entry for one frame of type \c CAL_REF which
 * contains the response and cavity gain frequency series that will be up
 * dated to the specified point in time. For example it must contain an entry
 * such as
 * \code
 * L CAL_REF 715388533 64 file://localhost/path/to/L-CAL_REF-715388533-64.gwf
 * \endcode
 * where the frame file contains the channels L1:CAL-RESPONSE and
 * L1:CAL-CAV_GAIN.
 *
 * It must also contain entries for the frames needed to update the
 * point calibration to the current time. These must contain the
 * L1:CAL-OLOOP_FAC and L1:CAL-CAV_FAC channels. The update
 * factor frames may either be SenseMon type frames, containing the factor
 * channels as \c real_8 trend data or frames generated by the lalapps
 * program \c lalapps_mkcalfac which creates channels of type
 * \c complex_8. The entries in the cache file must be of the format
 * \code
 * L CAL_FAC 714240000 1369980 file://localhost/path/to/L-CAL_FAC-714240000-1369980.gwf
 * \endcode
 * for \c lalapps_mkcalfac type frames or
 * \code
 * L SenseMonitor_L1_M 729925200 3600 file://localhost/path/to/L-SenseMonitor_L1_M-729925200-3600.gwf
 * \endcode
 * for SenseMon type frames.  If both types of frame are present in the cache,
 * SenseMon frames are used in preference.
 *
 */
/**@{*/

/**
 *
 * ### Error conditions ###
 *
 * These error conditions are generated by the function
 * ExtractFrameCalibration() if it encounters an error.
 * If codes 1 through 7 are returned, the no calibration has
 * been generated since there was an error reading the reference
 * calibration. If an error occours once the reference calibration
 * has been read in, then the reference calibration is returned
 * without being updated. This allows the user the option to
 * trap the error and fall back on the reference calibration or
 * to give up completely. This can be done in the case of error
 * code 8, or error code \f$-1\f$, which indicates an error in one
 * of the functions used to update the reference calibration.
 *
 */
/**\name Error Codes */
/**@{*/
#define FRAMECALIBRATIONH_ENULL 1
#define FRAMECALIBRATIONH_ENNUL 2
#define FRAMECALIBRATIONH_EMCHE 3
#define FRAMECALIBRATIONH_ECREF 4
#define FRAMECALIBRATIONH_EOREF 5
#define FRAMECALIBRATIONH_EDCHE 6
#define FRAMECALIBRATIONH_EREFR 7
#define FRAMECALIBRATIONH_ECFAC 8
#define FRAMECALIBRATIONH_EDTMM 9
#define FRAMECALIBRATIONH_EMETH 10

#define FRAMECALIBRATIONH_MSGENULL "No calibration generated: Null pointer"
#define FRAMECALIBRATIONH_MSGENNUL "No calibration generated: Non-null pointer"
#define FRAMECALIBRATIONH_MSGEMCHE "No calibration generated: unable to open calibration cache file"
#define FRAMECALIBRATIONH_MSGECREF "No calibration generated: no reference calibration in cache"
#define FRAMECALIBRATIONH_MSGEOREF "No calibration generated: unable to open reference frame"
#define FRAMECALIBRATIONH_MSGEDCHE "No calibration generated: error reference calibration cache"
#define FRAMECALIBRATIONH_MSGEREFR "No calibration generated: error reading refernce calibration"
#define FRAMECALIBRATIONH_MSGECFAC "Calibration not updated: no update factor frames in cache"
#define FRAMECALIBRATIONH_MSGEDTMM "Calibration not updated: mismatch between sample rate of alpha and alpha*beta"
#define FRAMECALIBRATIONH_MSGEMETH "Calibration cache must either be read from a file or globbed"
/**@}*/

void
LALExtractFrameResponse(LALStatus * status,
    COMPLEX8FrequencySeries * output,
    LALCache * calCache, CalibrationUpdateParams * calfacts);

void
LALCreateCalibFrCache(LALStatus * status,
    LALCache ** output,
    const CHAR * calCacheName,
    const CHAR * dirstr, const CHAR * calGlobPattern);

/**@}*/

#if 0
{       /* so that editors will match succeeding brace */
#elif defined(__cplusplus)
}
#endif

#endif /* _FRAMECALIBRATION_H */
