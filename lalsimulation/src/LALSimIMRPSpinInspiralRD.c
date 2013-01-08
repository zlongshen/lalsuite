/*
 * Copyright (C) 2011 Riccardo Sturani, John Veitch, Drew Keppel
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

#include <stdlib.h>
#include <math.h>
#include <gsl/gsl_linalg.h>
#include <gsl/gsl_interp.h>
#include <gsl/gsl_spline.h>
#include <lal/LALStdlib.h>
#include <lal/AVFactories.h>
#include <lal/SeqFactories.h>
#include <lal/Units.h>
#include <lal/TimeSeries.h>
#include <lal/LALConstants.h>
#include <lal/SeqFactories.h>
#include <lal/RealFFT.h>
#include <lal/SphericalHarmonics.h>
#include <lal/LALAdaptiveRungeKutta4.h>
#include <lal/LALSimInspiral.h>
#include <lal/Date.h>

#include "LALSimIMRPSpinInspiralRD.h"
#include "LALSimInspiralPNCoefficients.c"

/* Minimum integration length */
#define minIntLen    16
/* For turning on debugging messages*/
#define DEBUG_RD  0

#define nModes 8
#define RD_EFOLDS 10

static REAL8 dsign(int i){
  if (i>=0) return 1.;
  else return -1.;
}

static REAL8 OmMatch(REAL8 LNhS1, REAL8 LNhS2, REAL8 S1S1, REAL8 S1S2, REAL8 S2S2) {

  const REAL8 omM       = 0.0555;
  const REAL8 omMsz12   =    9.97e-4;
  const REAL8 omMs1d2   =  -2.032e-3;
  const REAL8 omMssq    =   5.629e-3;
  const REAL8 omMsz1d2  =   8.646e-3;
  const REAL8 omMszsq   =  -5.909e-3;
  const REAL8 omM3s1d2  =   1.801e-3;
  const REAL8 omM3ssq   = -1.4059e-2;
  const REAL8 omM3sz1d2 =  1.5483e-2;
  const REAL8 omM3szsq  =   8.922e-3;

  return omM + /*6.05e-3 * sqrtOneMinus4Eta +*/
    omMsz12   * (LNhS1 + LNhS2) +
    omMs1d2   * (S1S2) +
    omMssq    * (S1S1 + S2S2) +
    omMsz1d2  * (LNhS1 * LNhS2) +
    omMszsq   * (LNhS1 * LNhS1 + LNhS2 * LNhS2) +
    omM3s1d2  * (LNhS1 + LNhS2) * (S1S2) +
    omM3ssq   * (LNhS1 + LNhS2) * (S1S1+S2S2) +
    omM3sz1d2 * (LNhS1 + LNhS2) * (LNhS1*LNhS2) +
    omM3szsq  * (LNhS1 + LNhS2) * (LNhS1*LNhS1+LNhS2*LNhS2);
} /* End of OmMatch */

static REAL8 fracRD(REAL8 LNhS1, REAL8 LNhS2, REAL8 S1S1, REAL8 S1S2, REAL8 S2S2) {

  const double frac0      = 0.840;
  const double fracsz12   = -2.145e-2;
  const double fracs1d2   = -4.421e-2;
  const double fracssq    = -2.643e-2;
  const double fracsz1d2  = -5.876e-2;
  const double fracszsq   = -2.215e-2;

  return frac0 +
    fracsz12   * (LNhS1 + LNhS2) +
    fracs1d2   * (S1S2) +
    fracssq    * (S1S1 + S2S2) +
    fracsz1d2  * (LNhS1 * LNhS2) +
    fracszsq   * (LNhS1 * LNhS1 + LNhS2 * LNhS2);
} /* End of fracRD */

/**
 * Convenience function to set up XLALSimInspiralSpinTaylotT4Coeffs struct
 */

static int XLALSimIMRPhenSpinParamsSetup(LALSimInspiralSpinTaylorT4Coeffs  *params,  /** PN params [returned] */
					 REAL8 dt,                                   /** Sampling in secs */
					 REAL8 fStart,                               /** Starting frequency of integration*/
					 REAL8 fEnd,                                 /** Ending frequency of integration*/
					 REAL8 mass1,                                /** Mass 1 in solar mass units */
					 REAL8 mass2,                                /** Mass 2 in solar mass units */
					 LALSimInspiralInteraction interFlags,       /** Spin interaction */
					 LALSimInspiralTestGRParam *testGR,          /** Test GR param */
					 UINT4 order                                 /** twice PN Order in Phase */
					 )
{
  /* Zero the coefficients */
  memset(params, 0, sizeof(LALSimInspiralSpinTaylorT4Coeffs));
  params->M      = (mass1+mass2);
  params->eta    = mass1*mass2/(params->M*params->M);
  params->m1ByM  = mass1 / params->M;
  params->m2ByM  = mass2 / params->M;
  params->dmByM  = (mass1 - mass2) / params->M;
  params->m1Bym2 = mass1/mass2;
  params->M     *= LAL_MTSUN_SI;
  REAL8 unitHz   = params->M *((REAL8) LAL_PI);

  params->fEnd   = fEnd*unitHz;    /*On the left side there is actually an omega*/
  params->fStart = fStart*unitHz;  /*On the left side there is actually an omega*/
  if (fEnd>0.)
    params->dt     = dt*(fEnd-fStart)/fabs(fEnd-fStart);
  else
    params->dt     = dt;

  REAL8 phi1 = XLALSimInspiralTestGRParamExists(testGR,"phi1") ? XLALSimInspiralGetTestGRParam(testGR,"phi1") : 0.;
  REAL8 phi2 = XLALSimInspiralTestGRParamExists(testGR,"phi2") ? XLALSimInspiralGetTestGRParam(testGR,"phi2") : 0.;
  REAL8 phi3 = XLALSimInspiralTestGRParamExists(testGR,"phi3") ? XLALSimInspiralGetTestGRParam(testGR,"phi3") : 0.;
  REAL8 phi4 = XLALSimInspiralTestGRParamExists(testGR,"phi4") ? XLALSimInspiralGetTestGRParam(testGR,"phi4") : 0.;

  params->wdotnewt = XLALSimInspiralTaylorT4Phasing_0PNCoeff(params->eta);
  params->Enewt    = XLALSimInspiralEnergy_0PNCoeff(params->eta);

  switch (order) {

    case -1: // Use the highest PN order available.
    case 7:
      params->wdotcoeff[7]  = XLALSimInspiralTaylorT4Phasing_7PNCoeff(params->eta);

    case 6:
      params->Ecoeff[6]     = XLALSimInspiralEnergy_6PNCoeff(params->eta);
      params->wdotcoeff[6]  = XLALSimInspiralTaylorT4Phasing_6PNCoeff(params->eta);
      params->wdotlogcoeff  = XLALSimInspiralTaylorT4Phasing_6PNLogCoeff(params->eta);
      if( (interFlags & LAL_SIM_INSPIRAL_INTERACTION_SPIN_ORBIT_3PN) == LAL_SIM_INSPIRAL_INTERACTION_SPIN_ORBIT_3PN ) {
	params->wdotSO3s1   = XLALSimInspiralTaylorT4Phasing_6PNSLCoeff(params->m1ByM);
	params->wdotSO3s2   = XLALSimInspiralTaylorT4Phasing_6PNSLCoeff(params->m1ByM);
      }

    case 5:
      params->Ecoeff[5]     = 0.;
      params->wdotcoeff[5]  = XLALSimInspiralTaylorT4Phasing_5PNCoeff(params->eta);
      if( (interFlags & LAL_SIM_INSPIRAL_INTERACTION_SPIN_ORBIT_25PN) == LAL_SIM_INSPIRAL_INTERACTION_SPIN_ORBIT_25PN ) {
	params->ESO25s1     = XLALSimInspiralEnergy_5PNSOCoeffs1(params->eta, params->m1Bym2);
	params->ESO25s2     = XLALSimInspiralEnergy_5PNSOCoeffs1(params->eta, 1./params->m1Bym2);
	params->wdotSO25s1  = XLALSimInspiralTaylorT4Phasing_5PNSLCoeff(params->eta, params->m1Bym2);
	params->wdotSO25s2  = XLALSimInspiralTaylorT4Phasing_5PNSLCoeff(params->eta, 1./params->m1Bym2);
	params->S1dot25     = XLALSimInspiralSpinDot_5PNCoeff(params->eta,params->dmByM);
	params->S2dot25     = XLALSimInspiralSpinDot_5PNCoeff(params->eta,-params->dmByM);
      }

    case 4:
      params->wdotcoeff[4]  = XLALSimInspiralTaylorT4Phasing_4PNCoeff(params->eta)+phi4;
      params->Ecoeff[4]   = XLALSimInspiralEnergy_4PNCoeff(params->eta);
      if( (interFlags & LAL_SIM_INSPIRAL_INTERACTION_SPIN_SPIN_2PN) ==  LAL_SIM_INSPIRAL_INTERACTION_SPIN_SPIN_2PN ) {
	params->wdotSS2     = XLALSimInspiralTaylorT4Phasing_4PNSSCoeff(params->eta);
	params->wdotSSO2    = XLALSimInspiralTaylorT4Phasing_4PNSSOCoeff(params->eta);
	params->ESS2        = XLALSimInspiralEnergy_4PNSSCoeff(params->eta);
	params->ESSO2       = XLALSimInspiralEnergy_4PNSSOCoeff(params->eta);
      }
      if( (interFlags & LAL_SIM_INSPIRAL_INTERACTION_SPIN_SPIN_SELF_2PN) == LAL_SIM_INSPIRAL_INTERACTION_SPIN_SPIN_SELF_2PN ) {
	params->wdotSelfSS2 = XLALSimInspiralTaylorT4Phasing_4PNSelfSSCoeff(params->eta);
	params->wdotSelfSSO2= XLALSimInspiralTaylorT4Phasing_4PNSelfSSOCoeff(params->eta);
	params->ESelfSS2s1  = XLALSimInspiralEnergy_4PNSelfSSCoeff(params->m1Bym2);
	params->ESelfSS2s2  = XLALSimInspiralEnergy_4PNSelfSSCoeff(1./params->m1Bym2);
	params->ESelfSSO2s1 = XLALSimInspiralEnergy_4PNSelfSSOCoeff(params->m1Bym2);
	params->ESelfSSO2s2 = XLALSimInspiralEnergy_4PNSelfSSOCoeff(1./params->m1Bym2);
      }

    case 3:
      params->Ecoeff[3]      = 0.;
      params->wdotcoeff[3]   = XLALSimInspiralTaylorT4Phasing_3PNCoeff(params->eta)+phi3;
      if( (interFlags & LAL_SIM_INSPIRAL_INTERACTION_SPIN_ORBIT_15PN) == LAL_SIM_INSPIRAL_INTERACTION_SPIN_ORBIT_15PN ) {
	  params->wdotSO15s1 = XLALSimInspiralTaylorT4Phasing_3PNSOCoeff(params->m1Bym2);
	  params->wdotSO15s2 = XLALSimInspiralTaylorT4Phasing_3PNSOCoeff(1./params->m1Bym2);
	  params->ESO15s1    = XLALSimInspiralEnergy_3PNSOCoeff(params->m1Bym2);
	  params->ESO15s2    = XLALSimInspiralEnergy_3PNSOCoeff(1./params->m1Bym2);
	  params->S1dot15    = XLALSimInspiralSpinDot_3PNCoeff(params->eta,params->m1Bym2);
	  params->S2dot15    = XLALSimInspiralSpinDot_3PNCoeff(params->eta,1./params->m1Bym2);
      }

    case 2:
      params->Ecoeff[2]  = XLALSimInspiralEnergy_2PNCoeff(params->eta);
      params->wdotcoeff[2] = XLALSimInspiralTaylorT4Phasing_2PNCoeff(params->eta)+phi2;

    case 1:
      params->Ecoeff[1]  = 0.;
      params->wdotcoeff[1] = phi1;

    case 0:
      params->Ecoeff[0]  = 1.;
      params->wdotcoeff[0] = 1.;
      break;

    case 8:
      XLALPrintError("*** LALSimIMRPhenSpinInspiralRD ERROR: PhenSpin approximant not available at pseudo4PN order\n");
			XLAL_ERROR(XLAL_EDOM);
      break;

    default:
      XLALPrintError("*** LALSimIMRPhenSpinInspiralRD ERROR: Impossible to create waveform with %d order\n",order);
			XLAL_ERROR(XLAL_EFAILED);
      break;
  }

  return XLAL_SUCCESS;
} /* End of XLALSimIMRPhenSpinParamsSetup */

static int XLALSpinInspiralDerivatives(UNUSED double t,
				       const double values[],
				       double dvalues[],
				       void *mparams)
{
  REAL8 omega;                // time-derivative of the orbital phase
  REAL8 LNhx, LNhy, LNhz;     // orbital angular momentum unit vector
  REAL8 S1x, S1y, S1z;        // dimension-less spin variable S/M^2
  REAL8 S2x, S2y, S2z;
  REAL8 LNhS1, LNhS2;         // scalar products
  REAL8 domega;               // derivative of omega
  REAL8 dLNhx, dLNhy, dLNhz;  // derivatives of \f$\hat L_N\f$ components
  REAL8 dS1x, dS1y, dS1z;     // derivative of \f$S_i\f$
  REAL8 dS2x, dS2y, dS2z;
  REAL8 energy,energyold;

  /* auxiliary variables*/
  REAL8 S1S2, S1S1, S2S2;     // Scalar products
  REAL8 alphadotcosi;         // alpha is the right ascension of L, i(iota) the angle between L and J
  REAL8 v, v2, v4, v5, v6, v7;
  REAL8 tmpx, tmpy, tmpz, cross1x, cross1y, cross1z, cross2x, cross2y, cross2z, LNhxy;

  LALSimInspiralSpinTaylorT4Coeffs *params = (LALSimInspiralSpinTaylorT4Coeffs *) mparams;

  /* --- computation start here --- */
  omega = values[1];

  LNhx = values[2];
  LNhy = values[3];
  LNhz = values[4];

  S1x = values[5];
  S1y = values[6];
  S1z = values[7];

  S2x = values[8];
  S2y = values[9];
  S2z = values[10];

  energyold = values[11];

  v = cbrt(omega);
  v2 = v * v;
  v4 = omega * v;
  v5 = omega * v2;
  v6 = omega * omega;
  v7 = omega * omega * v;

  // Omega derivative without spin effects up to 3.5 PN
  // this formula does not include the 1.5PN shift mentioned in arXiv:0810.5336, five lines below (3.11)
  domega = params->wdotcoeff[0]
          + v * (params->wdotcoeff[1]
                 + v * (params->wdotcoeff[2]
                        + v * (params->wdotcoeff[3]
                               + v * (params->wdotcoeff[4]
                                      + v * (params->wdotcoeff[5]
                                             + v * (params->wdotcoeff[6] + params->wdotlogcoeff * log(omega)
                                                    + v * params->wdotcoeff[7]))))));

  energy = (params->Ecoeff[0] + v2 * (params->Ecoeff[2] +
				      v2 * (params->Ecoeff[4] +
					    v2 * params->Ecoeff[6])));

  // Adding spin effects
  // L dot S1,2
  LNhS1 = (LNhx * S1x + LNhy * S1y + LNhz * S1z);
  LNhS2 = (LNhx * S2x + LNhy * S2y + LNhz * S2z);

  // wdotSO15si = -1/12 (...)
  domega += omega * (params->wdotSO15s1 * LNhS1 + params->wdotSO15s2 * LNhS2); // see e.g. Buonanno et al. gr-qc/0211087

  energy += omega * (params->ESO15s1 * LNhS1 + params->ESO15s2 * LNhS2);  // see e.g. Blanchet et al. gr-qc/0605140

  // wdotSS2 = -1/48 eta ...
  S1S1 = (S1x * S1x + S1y * S1y + S1z * S1z);
  S2S2 = (S2x * S2x + S2y * S2y + S2z * S2z);
  S1S2 = (S1x * S2x + S1y * S2y + S1z * S2z);
  domega += v4 * ( params->wdotSS2 * S1S2 + params->wdotSSO2 * LNhS1 * LNhS2);	// see e.g. Buonanno et al. arXiv:0810.5336
  domega += v4 * ( params->wdotSelfSSO2 * (LNhS1 * LNhS1 + LNhS2 * LNhS2) + params->wdotSelfSS2 * (S1S1 + S2S2));
  // see Racine et al. arXiv:0812.4413

  energy += v4 * (params->ESS2 * S1S2 + params->ESSO2 * LNhS1 * LNhS2);    // see e.g. Buonanno et al. as above
  energy += v4 * (params->ESelfSS2s1 * S1S1 + params->ESelfSS2s2 * S2S2 + params->ESelfSSO2s1 * LNhS1 * LNhS1 + params->ESelfSSO2s2 * LNhS2 * LNhS2);	// see Racine et al. as above

  // wdotspin25SiLNh = see below
  domega += v5 * (params->wdotSO25s1 * LNhS1 + params->wdotSO25s2 * LNhS2);	//see (8.3) of Blanchet et al.
  energy += v5 * (params->ESO25s1 * LNhS1 + params->ESO25s2 * LNhS2);    //see (7.9) of Blanchet et al.

  domega += omega*omega * (params->wdotSO3s1 * LNhS1 + params->wdotSO3s2 * LNhS2); // see (6.5) of arXiv:1104.5659

  // Setting the right pre-factor
  domega *= params->wdotnewt * v5 * v6;
  energy *= params->Enewt * v2;

  /*Derivative of the angular momentum and spins */

  /* dS1, 1.5PN */
  /* S1dot15= (4+3m2/m1)/2 * eta */
  cross1x = (LNhy * S1z - LNhz * S1y);
  cross1y = (LNhz * S1x - LNhx * S1z);
  cross1z = (LNhx * S1y - LNhy * S1x);

  dS1x = params->S1dot15 * v5 * cross1x;
  dS1y = params->S1dot15 * v5 * cross1y;
  dS1z = params->S1dot15 * v5 * cross1z;

  /* dS1, 2PN */
  /* Sdot20= 0.5 */
  tmpx = S1z * S2y - S1y * S2z;
  tmpy = S1x * S2z - S1z * S2x;
  tmpz = S1y * S2x - S1x * S2y;

  // S1S2 contribution see. eq. 2.23 of arXiv:0812.4413
  dS1x += 0.5 * v6 * (tmpx - 3. * LNhS2 * cross1x);
  dS1y += 0.5 * v6 * (tmpy - 3. * LNhS2 * cross1y);
  dS1z += 0.5 * v6 * (tmpz - 3. * LNhS2 * cross1z);
  // S1S1 contribution
  dS1x -= 1.5 * v6 * LNhS1 * cross1x * (1. + 1./params->m1Bym2) * params->m1ByM;
  dS1y -= 1.5 * v6 * LNhS1 * cross1y * (1. + 1./params->m1Bym2) * params->m1ByM;
  dS1z -= 1.5 * v6 * LNhS1 * cross1z * (1. + 1./params->m1Bym2) * params->m1ByM;

  // dS1, 2.5PN, eq. 7.8 of Blanchet et al. gr-qc/0605140
  // S1dot25= 9/8-eta/2.+eta+mparams->eta*29./24.+mparams->m1m2*(-9./8.+5./4.*mparams->eta)
  dS1x += params->S1dot25 * v7 * cross1x;
  dS1y += params->S1dot25 * v7 * cross1y;
  dS1z += params->S1dot25 * v7 * cross1z;

  /* dS2, 1.5PN */
  cross2x = (LNhy * S2z - LNhz * S2y);
  cross2y = (LNhz * S2x - LNhx * S2z);
  cross2z = (LNhx * S2y - LNhy * S2x);

  dS2x = params->S2dot15 * v5 * cross2x;
  dS2y = params->S2dot15 * v5 * cross2y;
  dS2z = params->S2dot15 * v5 * cross2z;

  /* dS2, 2PN */
  dS2x += 0.5 * v6 * (-tmpx - 3.0 * LNhS1 * cross2x);
  dS2y += 0.5 * v6 * (-tmpy - 3.0 * LNhS1 * cross2y);
  dS2z += 0.5 * v6 * (-tmpz - 3.0 * LNhS1 * cross2z);
  // S2S2 contribution
  dS2x -= 1.5 * v6 * LNhS2 * cross2x * (1. + params->m1Bym2) * params->m2ByM;
  dS2y -= 1.5 * v6 * LNhS2 * cross2y * (1. + params->m1Bym2) * params->m2ByM;
  dS2z -= 1.5 * v6 * LNhS2 * cross2z * (1. + params->m1Bym2) * params->m2ByM;

  // dS2, 2.5PN, eq. 7.8 of Blanchet et al. gr-qc/0605140
  dS2x += params->S2dot25 * v7 * cross2x;
  dS2y += params->S2dot25 * v7 * cross2y;
  dS2z += params->S2dot25 * v7 * cross2z;

  dLNhx = -(dS1x + dS2x) * v / params->eta;
  dLNhy = -(dS1y + dS2y) * v / params->eta;
  dLNhz = -(dS1z + dS2z) * v / params->eta;

  /* dphi */
  LNhxy = LNhx * LNhx + LNhy * LNhy;

  if (LNhxy > 0.0)
    alphadotcosi = LNhz * (LNhx * dLNhy - LNhy * dLNhx) / LNhxy;
  else
  {
    XLALPrintWarning("*** LALPSpinInspiralRD WARNING ***: alphadot set to 0, LNh:(%12.4e %12.4e %12.4e)\n",LNhx,LNhy,LNhz);
    alphadotcosi = 0.;
  }

  /* dvalues->data[0] is the phase derivative */
  /* omega is the derivative of the orbital phase omega \neq dvalues->data[0] */
  dvalues[0] = omega - alphadotcosi;
  dvalues[1] = domega;

  dvalues[2] = dLNhx;
  dvalues[3] = dLNhy;
  dvalues[4] = dLNhz;

  dvalues[5] = dS1x;
  dvalues[6] = dS1y;
  dvalues[7] = dS1z;

  dvalues[8] = dS2x;
  dvalues[9] = dS2y;
  dvalues[10] = dS2z;

  dvalues[11] = (energy-energyold)/params->dt*params->M;

  return GSL_SUCCESS;
} /* end of XLALSpinInspiralDerivatives */

int XLALGenerateWaveDerivative (REAL8Vector *dwave,
				REAL8Vector *wave,
				REAL8 dt
				)
{
  /* XLAL error handling */
  int errcode = XLAL_SUCCESS;

  /* For checking GSL return codes */
  INT4 gslStatus;

  UINT4 j;
  double *x, *y;
  double dy;
  gsl_interp_accel *acc;
  gsl_spline *spline;

  if (wave->length!=dwave->length)
    XLAL_ERROR( XLAL_EFUNC );

  /* Getting interpolation and derivatives of the waveform using gsl spline routine */
  /* Initialize arrays and supporting variables for gsl */

  x = (double *) LALMalloc(wave->length * sizeof(double));
  y = (double *) LALMalloc(wave->length * sizeof(double));

  if ( !x || !y )
  {
    if ( x ) LALFree (x);
    if ( y ) LALFree (y);
    XLAL_ERROR( XLAL_ENOMEM );
  }

  for (j = 0; j < wave->length; ++j)
  {
		x[j] = j;
		y[j] = wave->data[j];
  }

  XLAL_CALLGSL( acc = (gsl_interp_accel*) gsl_interp_accel_alloc() );
  XLAL_CALLGSL( spline = (gsl_spline*) gsl_spline_alloc(gsl_interp_cspline, wave->length) );
  if ( !acc || !spline )
  {
    if ( acc )    gsl_interp_accel_free(acc);
    if ( spline ) gsl_spline_free(spline);
    LALFree( x );
    LALFree( y );
    XLAL_ERROR( XLAL_ENOMEM );
  }

  /* Gall gsl spline interpolation */
  XLAL_CALLGSL( gslStatus = gsl_spline_init(spline, x, y, wave->length) );
  if ( gslStatus != GSL_SUCCESS )
  {
    gsl_spline_free(spline);
    gsl_interp_accel_free(acc);
    LALFree( x );
    LALFree( y );
    XLAL_ERROR( XLAL_EFUNC );
  }

  /* Getting first and second order time derivatives from gsl interpolations */
  for (j = 0; j < wave->length; ++j)
  {
    XLAL_CALLGSL(gslStatus = gsl_spline_eval_deriv_e( spline, j, acc, &dy ) );
    if (gslStatus != GSL_SUCCESS )
    {
      gsl_spline_free(spline);
      gsl_interp_accel_free(acc);
      LALFree( x );
      LALFree( y );
      XLAL_ERROR( XLAL_EFUNC );
    }
    dwave->data[j]  = (REAL8)(dy / dt);
  }

  /* Free gsl variables */
  gsl_spline_free(spline);
  gsl_interp_accel_free(acc);
  LALFree(x);
  LALFree(y);
	
  return errcode;
} /* End of XLALGenerateWaveDerivative */

static int XLALSimSpinInspiralTest(UNUSED double t, const double values[], double dvalues[], void *mparams) {

  LALSimInspiralSpinTaylorT4Coeffs *params = (LALSimInspiralSpinTaylorT4Coeffs *) mparams;

  REAL8 omega   =   values[1];
  REAL8 energy  =  values[11];
  REAL8 denergy = dvalues[11];

  if ( (energy > 0.0) || (( denergy*params->dt/params->M > - 0.001*energy )&&(energy<0.) ) ) {
    if (energy>0.) XLALPrintWarning("*** Test: LALSimIMRPSpinInspiral WARNING **: Bounding energy >ve!\n");
    else 
      XLALPrintWarning("*** Test: LALSimIMRPSpinInspiral WARNING **:  Energy increases dE %12.6e dE*dt %12.6e 1pMEn %12.4e M: %12.4e, eta: %12.4e  om %12.6e \n", denergy, denergy*params->dt/params->M, - 0.001*energy, params->M/LAL_MTSUN_SI, params->eta, omega);
    return LALSIMINSPIRAL_PHENSPIN_TEST_ENERGY;
  }
  else if (omega < 0.0) {
    XLALPrintWarning("** LALSimIMRPSpinInspiral WARNING **: omega < 0  M: %12.4e, eta: %12.4e  om %12.6e\n",params->M, params->eta, omega);
    return LALSIMINSPIRAL_PHENSPIN_TEST_OMEGANONPOS;
  }
  else if (dvalues[1] < 0.0) {
    /* omegadot < 0 */
    return LALSIMINSPIRAL_PHENSPIN_TEST_OMEGADOT;
  }
  else if (isnan(omega)) {
    /* omega is nan */
    return LALSIMINSPIRAL_PHENSPIN_TEST_OMEGANAN;
  } 
  else if ( params->fEnd > 0. && params->fStart > params->fEnd && omega < params->fEnd) {
    /* freq. below bound in backward integration */
    return LALSIMINSPIRAL_PHENSPIN_TEST_FREQBOUND;
  }
  else if ( params->fEnd > params->fStart && omega > params->fEnd) {
    /* freq. above bound in forward integration */
    return LALSIMINSPIRAL_PHENSPIN_TEST_FREQBOUND;
  }
  else
    return GSL_SUCCESS;
} /* End of XLALSimSpinInspiralTest */


static int XLALSimIMRPhenSpinTest(UNUSED double t, const double values[], double dvalues[], void *mparams) {

  LALSimInspiralSpinTaylorT4Coeffs *params = (LALSimInspiralSpinTaylorT4Coeffs *) mparams;

  REAL8 omega   =   values[1];
  REAL8 energy  =  values[11];
  REAL8 denergy = dvalues[11];

  REAL8 LNhS1=(values[2]*values[5]+values[3]*values[6]+values[4]*values[7])/params->m1ByM/params->m1ByM;
  REAL8 LNhS2=(values[2]*values[8]+values[3]*values[9]+values[4]*values[10])/params->m2ByM/params->m2ByM;
  REAL8 S1sq =(values[5]*values[5]+values[6]*values[6]+values[7]*values[7])/pow(params->m1ByM,4);
  REAL8 S2sq =(values[8]*values[8]+values[9]*values[9]+values[10]*values[10])/pow(params->m2ByM,4);
  REAL8 S1S2 =(values[5]*values[8]+values[6]*values[9]+values[7]*values[10])/pow(params->m1ByM*params->m2ByM,2);
	
  REAL8 omegaMatch=OmMatch(LNhS1,LNhS2,S1sq,S1S2,S2sq)+0.006;

  if ( (energy > 0.0) || (( denergy*params->dt/params->M > - 0.001*energy )&&(energy<0.) ) ) {
    if (energy>0.) XLALPrintWarning("*** Test: LALSimIMRPSpinInspiralRD WARNING **: Bounding energy >ve!\n");
    else 
      XLALPrintWarning("*** Test: LALSimIMRPSpinInspiralRD WARNING **:  Energy increases dE %12.6e dE*dt %12.6e 1pMEn %12.4e M: %12.4e, eta: %12.4e  om %12.6e \n", denergy, denergy*params->dt/params->M, - 0.001*energy, params->M/LAL_MTSUN_SI, params->eta, omega);
    return LALSIMINSPIRAL_PHENSPIN_TEST_ENERGY;
  }
  else if (omega < 0.0) {
    XLALPrintWarning("** LALSimIMRPSpinInspiralRD WARNING **: omega < 0  M: %12.4e, eta: %12.4e  om %12.6e\n",params->M, params->eta, omega);
    return LALSIMINSPIRAL_PHENSPIN_TEST_OMEGANONPOS;
  }
  else if (dvalues[1] < 0.0) {
    /* omegadot < 0 */
    return LALSIMINSPIRAL_PHENSPIN_TEST_OMEGADOT;
  }
  else if (isnan(omega)) {
    /* omega is nan */
    return LALSIMINSPIRAL_PHENSPIN_TEST_OMEGANAN;
  } 
  else if ( params->fEnd > 0. && params->fStart > params->fEnd && omega < params->fEnd) {
    /* freq. below bound in backward integration */
    return LALSIMINSPIRAL_PHENSPIN_TEST_FREQBOUND;
  }
  else if ( params->fEnd > params->fStart && omega > params->fEnd) {
    /* freq. above bound in forward integration */
    return LALSIMINSPIRAL_PHENSPIN_TEST_FREQBOUND;
  }
  else if (omega>omegaMatch) {
    return LALSIMINSPIRAL_PHENSPIN_TEST_OMEGAMATCH;
  }
  else
    return GSL_SUCCESS;
} /* End of XLALSimIMRPhenSpinTest */

int XLALSimSpinInspiralFillL2Modes(COMPLEX16Vector *hL2,
				   REAL8 v,
				   REAL8 eta,
				   REAL8 dm,
				   REAL8 Psi,
				   REAL8 alpha,
				   LALSimInspiralInclAngle *an
				   )
{
  const int os=2;
  REAL8 amp20 = sqrt(1.5);
  REAL8 v2    = v*v;
  REAL8 damp  = 1.;

  hL2->data[2+os] = ( ( 1. - damp * v2 / 42. * (107. - 55. * eta) ) * 
		      ( cos(2.*(Psi+alpha)) * an->cHi4 + cos(2.*(Psi-alpha)) * an->sHi4 ) + 
		      v * dm/3.*an->si * ( cos(Psi-2.*alpha) * an->sHi2 + cos(Psi + 2.*alpha) * an->cHi2 ) );

  hL2->data[2+os]+=I*( ( 1. - damp * v2 / 42. * (107. - 55. * eta) ) * 
		       (-sin(2.*(Psi+alpha)) * an->cHi4 + sin(2.*(Psi-alpha)) * an->sHi4 ) +  
		       v * dm/3.*an->si * ( sin(Psi-2.*alpha) * an->sHi2 - sin(Psi + 2. * alpha) * an->cHi2 ) );

  hL2->data[-2+os] = ( ( 1. - damp * v2 / 42. * (107. - 55. * eta) ) *
		       ( cos(2. * (Psi + alpha)) * an->cHi4 + cos(2. * (Psi - alpha)) * an->sHi4 ) -
		       v * dm / 3. * an->si * ( cos(Psi - 2. * alpha) * an->sHi2 + cos(Psi + 2. * alpha) * an->cHi2 ) );

  hL2->data[-2+os]+=I*( ( 1. - damp * v2 / 42. * (107. - 55. * eta) ) *
			( sin(2.*(Psi + alpha))*an->cHi4 - sin(2.*(Psi-alpha)) * an->sHi4 ) +
			v*dm/3.*an->si * ( sin(Psi-2.*alpha) * an->sHi2 - sin(Psi+2.*alpha) * an->cHi2 ) );

  hL2->data[1+os] = an->si * ( ( 1. - damp * v2 / 42. * (107. - 55. * eta) ) *
			       ( -cos(2. * Psi - alpha) * an->sHi2 + cos(2. * Psi + alpha) * an->cHi2 ) +
			       v * dm / 3. * ( -cos(Psi + alpha) * (an->ci + an->cDi)/2. - cos(Psi - alpha) * an->sHi2 * (1. + 2. * an->ci) ) );

  hL2->data[1+os]+= an->si *I*( ( 1. - damp * v2 / 42. * (107. - 55. * eta) ) *
				( -sin(2.*Psi-alpha ) * an->sHi2 - sin(2.*Psi + alpha) * an->cHi2 ) +
				v * dm / 3. * (sin(Psi + alpha) * (an->ci + an->cDi)/2. - sin(Psi - alpha) * an->sHi2 * (1.+2.*an->ci) ) );

  hL2->data[-1+os] = an->si * ( ( 1. - damp * v2 / 42. * (107. - 55. * eta) ) * 
				( cos(2.*Psi-alpha) * an->sHi2 - cos(2.*Psi+alpha)*an->cHi2) +
				v * dm / 3. * ( -cos(Psi + alpha) * (an->ci + an->cDi)/2. - cos(Psi - alpha) * an->sHi2 * (1. + 2. * an->ci) ) );

  hL2->data[-1+os]+= an->si *I*( ( 1. - damp * v2 / 42. * (107. - 55. * eta) ) * 
				 ( -sin(2. * Psi - alpha) * an->sHi2 - sin(2. * Psi + alpha) * an->cHi2 ) -
				 v * dm / 3. * ( sin(Psi + alpha) * (an->ci + an->cDi)/2. - sin(Psi - alpha) * an->sHi2 * (1. + 2. * an->ci) ) );

  hL2->data[os] = amp20 * ( an->si2*( 1.- damp*v2/42.*(107.-55.*eta) )*cos(2.*Psi) + I*v*dm/3.*an->sDi*sin(Psi) );

  return XLAL_SUCCESS;
} /* End of XLALSimSpinInspiralFillL2Modes*/

int XLALSimSpinInspiralFillL3Modes(COMPLEX16Vector *hL3,
				   REAL8 v,
				   REAL8 eta,
				   REAL8 dm,
				   REAL8 Psi,
				   REAL8 alpha,
				   LALSimInspiralInclAngle *an)
{
  const int os=3;
  REAL8 amp32 = sqrt(1.5);
  REAL8 amp31 = sqrt(0.15);
  REAL8 amp30 = 1. / sqrt(5)/2.;
  REAL8 v2    = v*v;

  hL3->data[3+os] = (v * dm * (-9.*cos(3.*(Psi-alpha))*an->sHi6 - cos(Psi-3.*alpha)*an->sHi4*an->cHi2 + cos(Psi+3.*alpha)*an->sHi2*an->cHi4 + 9.*cos(3.*(Psi+alpha))*an->cHi6) +
		     v2 * 4. * an->si *(1.-3.*eta)* ( -cos(2.*Psi-3.*alpha)*an->sHi4 + cos(2.*Psi+3.*alpha)*an->cHi4) );

  hL3->data[3+os]+= I*(v * dm * (-9.*sin(3.*(Psi-alpha))*an->sHi6 - sin(Psi-3.*alpha)*an->sHi4*an->cHi2 - sin(Psi+3.*alpha)*an->sHi2*an->cHi4 - 9.*sin(3.*(Psi+alpha))* an->cHi6) +
		       v2 * 4. * an->si *(1.-3.*eta)* ( -sin(2.*Psi-3.*alpha)*an->sHi4 -sin(2.*Psi+3.*alpha)*an->cHi4) );
  
  hL3->data[-3+os] = (-v * dm * (-9.*cos(3.*(Psi-alpha))*an->sHi6 - cos(Psi-3.*alpha)*an->sHi4*an->cHi2 + cos(Psi+3.*alpha)*an->sHi2*an->cHi4 + 9.*cos(3.*(Psi+alpha))*an->cHi6) +
		      v2 * 4. * an->si *(1.-3.*eta)*( -cos(2.*Psi-3.*alpha)*an->sHi4 + cos(2.*Psi+3.*alpha)*an->cHi4) );

  hL3->data[-3+os]+=I*(v * dm *(-9.*sin(3.*(Psi-alpha))*an->sHi6 - sin(Psi-3.*alpha)*an->sHi4*an->cHi2 - sin(Psi+3.*alpha)*an->sHi2*an->cHi4 - 9.*sin(3.*(Psi+alpha))* an->cHi6) -
		       v2 * 4. * an->si * (1.-3.*eta)*( -sin(2.*Psi-3.*alpha)*an->sHi4 - sin(2.*Psi+3.*alpha)*an->cHi4 ) );

  hL3->data[2+os] = amp32 * ( v * dm/3. * (27.*cos(3.*Psi-2.*alpha)*an->si*an->sHi4 + 27.*cos(3.*Psi+2.*alpha)*an->si*an->cHi4 + cos(Psi+2.*alpha)*an->cHi3*(5.*an->sHi-3.*an->si*an->cHi-3.*an->ci*an->sHi) /2. + cos(Psi-2.*alpha)*an->sHi3*(5.*an->cHi+3.*an->ci*an->cHi-3.*an->si*an->sHi) /2. ) +
			      v2*(1./3.-eta) * (-8.*an->cHi4*(3.*an->ci-2.)*cos(2.*(Psi+alpha)) + 8.*an->sHi4*(3.*an->ci+2.)*cos(2.*(Psi-alpha)) ) );

  hL3->data[2+os]+= amp32*I*( v * dm/3. * ( 27.*sin(3.*Psi-2.*alpha)*an->si*an->sHi4 - 27.*cos(3.*Psi+2.*alpha)*an->si*an->cHi4 - sin(Psi+2.*alpha)*an->cHi3*(5.*an->sHi-3.*an->si*an->cHi-3.*an->ci*an->sHi) /2. + sin(Psi-2.*alpha)*an->sHi3*(5.*an->cHi+3.*an->ci*an->cHi-3.*an->si*an->sHi)/2. ) +
			      v2*(1./3.-eta) * ( 8.*an->cHi4*(3.*an->ci-2.)*sin(2.*(Psi+alpha)) + 8.*an->sHi4*(3.*an->ci+2.)*sin(2.*(Psi-alpha)) ) );

  hL3->data[-2+os] = amp32 * ( v * dm/3. * (27.*cos(3.*Psi-2.*alpha)*an->si*an->sHi4 + 27.*cos(3.*Psi+2.*alpha)*an->si*an->cHi4 + cos(Psi+2.*alpha)*an->cHi3*(5.*an->sHi-3.*an->si*an->cHi-3.*an->ci*an->sHi) /2. + cos(Psi-2.*alpha)*an->sHi3*(5.*an->cHi+3.*an->ci*an->cHi-3.*an->si*an->sHi) /2. ) - 
			       v2*(1./3.-eta) * ( 8.*an->cHi4*(3.*an->ci-2.)*cos(2.*(Psi+alpha)) - 8.*an->sHi4*(3.*an->ci+2.)*cos(2.*(Psi-alpha)) ) );

  hL3->data[-2+os]+= amp32*I*(-v * dm/3. * (27.*sin(3.*Psi-2.*alpha)*an->si*an->sHi4 - 27.*cos(3.*Psi+2.*alpha)*an->si*an->cHi4 - sin(Psi+2.*alpha)*an->cHi3*(5.*an->sHi-3.*an->si*an->cHi-3.*an->ci*an->sHi) /2.+ sin(Psi-2.*alpha)*an->sHi3*(5.*an->cHi+3.*an->ci*an->cHi-3.*an->si*an->sHi) /2.) +
			      v2*(1./3.-eta) * (8.*an->cHi4*(3.*an->ci-2.)*sin(2.*(Psi+alpha)) + 8.*an->sHi4*(3.*an->ci+2.)*sin(2.*(Psi-alpha)) ) );

  hL3->data[1+os] = amp31 * ( v * dm/6. * ( -135.*cos(3.*Psi-alpha)*an->sHi*an->sHi2 + 135.*cos(3.*Psi+alpha)*an->sHi*an->cHi2 + cos(Psi+alpha)*an->cHi2*(15.*an->cDi-20.*an->ci+13.)/2. - cos(Psi-alpha)*an->sHi2*(15.*an->cDi+20.*an->ci+13.)/2.)
			      + v2*(1./3.-eta) * ( 20.*an->cHi3*cos(2.*Psi+alpha)*(3.*(an->sHi*an->ci+an->cHi*an->si)-5.*an->sHi) + 20.*an->sHi3*cos(2.*Psi-alpha)*(3.*(an->cHi2*an->ci-an->sHi*an->si)+5.*an->cHi) ) );

  hL3->data[1+os]+= amp31*I*(-v * dm/6. * ( -135.*cos(3.*Psi-alpha)*an->si2*an->sHi2 + 135.*cos(3.*Psi+alpha)*an->si2*an->cHi2 + cos(Psi+alpha)*an->cHi2*(15.*an->cDi-20.*an->ci+13.)/2. - cos(Psi-alpha)*an->sHi2*(15.*an->cDi+20.*an->ci+13.)/2. )
			     - v2*(1./3.-eta) * ( 20.*an->cHi3*cos(2.*Psi+alpha)*(3.*(an->sHi*an->ci+an->cHi*an->si)-5.*an->sHi) + 20.*an->sHi3*cos(2.*Psi-alpha)*(3.*(an->cHi*an->ci-an->sHi*an->si)+5.*an->cHi) ) );

  hL3->data[-1+os] = amp31 * (-v * dm/6. * ( -135.*cos(3.*Psi-alpha)*an->si2*an->sHi2 + 135.*cos(3.*Psi+alpha)*an->si2*an->cHi2 + cos(Psi+alpha)*an->cHi2*(15.*an->cDi-20.*an->ci+13.)/2.- cos(Psi-alpha) * an->sHi2*(15.*an->cDi+20.*an->ci+13.)/2. ) -
			      v2 * (1./3.-eta)* ( 20.*an->cHi3*cos(2.*Psi+alpha)*(3.*(an->sHi*an->ci+an->cHi*an->si)-5.*an->sHi) + 20.*an->sHi3*cos(2.*Psi-alpha)*(3.*(an->cHi*an->ci-an->sHi*an->si)+5.*an->cHi) ) );

  hL3->data[-1+os]+= amp31*I*(v * dm/6. * ( -135.*sin(3.*Psi-alpha)*an->si2*an->sHi2 - 135.*sin(3.*Psi+alpha)*an->si2*an->cHi2 - sin(Psi+alpha)*an->cHi2*(15.*an->cDi-20.*an->ci+13.)/2. - sin(Psi-alpha)*an->sHi2*(15.*an->cDi+20.*an->ci+13.)/2.) 
			      -v2 * (1./3.-eta)* ( 20.*an->cHi3*sin(2.*Psi+alpha)*(3.*(an->sHi*an->ci+an->ci2*an->si)-5.*an->si2) - 20.*an->sHi3*sin(2.*Psi-alpha)*(3.*(an->ci2*an->ci-an->si2*an->si)+5.*an->ci2) ) );

  hL3->data[os] = amp30 * I * ( v * dm * ( cos(Psi)*an->si*(cos(2.*Psi)*(45.*an->si2)-(25.*an->cDi-21.) ) ) +
				v2*(1.-3.*eta) * (80.*an->si2*an->cHi*sin(2.*Psi) ) );

  return XLAL_SUCCESS;

} /*End of XLALSimSpinInspiralFillL3Modes*/


int XLALSimSpinInspiralFillL4Modes(COMPLEX16Vector *hL4,
				   UNUSED REAL8 v,
				   REAL8 eta,
				   UNUSED REAL8 dm,
				   REAL8 Psi,
				   REAL8 alpha,
				   LALSimInspiralInclAngle *an
				   )
{
  const int os=4;
  REAL8 amp43 = - sqrt(2.);
  REAL8 amp42 = sqrt(7.)/2.;
  REAL8 amp41 = sqrt(3.5)/4.;
  REAL8 amp40 = sqrt(17.5)/16.;
	
  hL4->data[4+os] = (1. - 3.*eta) * ( 4.*an->sHi8*cos(4.*(Psi-alpha)) + cos(2.*Psi-4.*alpha)*an->sHi6*an->cHi2 + an->sHi2*an->cHi6*cos(2.*Psi+4.*alpha) + 4.*an->cHi8*cos(4.*(Psi+alpha)) );

  hL4->data[4+os]+= (1. - 3.*eta)*I*( 4.*an->sHi8*sin(4.*(Psi-alpha)) + sin(2.*Psi-4.*alpha)*an->sHi6*an->cHi2 - an->sHi2*an->cHi6*sin(2.*Psi+4.*alpha) - 4.*an->cHi8*sin(4.*(Psi+alpha)) );

  hL4->data[-4+os] = (1. - 3.*eta) * (4.*an->sHi8*cos(4.*(Psi-alpha)) + cos(2.*Psi-4.*alpha)*an->sHi6*an->cHi2 + an->sHi2*an->cHi6*cos(2.*Psi+4.*alpha) + 4.*an->cHi8*cos(4.*(Psi+alpha) ) );

  hL4->data[-4+os]+=-(1. - 3.*eta) *I*(4.*an->sHi8*sin(4.*(Psi-alpha)) + sin(2*Psi-4.*alpha)*an->sHi6*an->cHi2 - an->sHi2*an->cHi6*sin(2.*Psi+4.*alpha) - 4.*an->cHi8*sin(4.*(Psi+alpha)) );

  hL4->data[3+os] = amp43 * (1. - 3.*eta) * an->si * ( 4.*an->sHi6*cos(4.*Psi-3.*alpha) - 4.*an->cHi6*cos(4.*Psi+3.*alpha) - an->sHi4*(an->ci+0.5)/2.*cos(2.*Psi-3.*alpha) + an->cHi4*(an->ci-0.5)*cos(2.*Psi+3.*alpha) ); /****/

  hL4->data[3+os]+= amp43*I*(1. - 3.*eta) * an->si * ( 4.*an->sHi6*sin(4.*Psi-3.*alpha) + 4.*an->cHi6*sin(4.*Psi+3.*alpha) - an->sHi4*(an->ci+0.5)/2.*sin(2.*Psi-3.*alpha) + an->cHi4*(an->ci-0.5)*sin(2.*Psi+3.*alpha) ); /****/

  hL4->data[-3+os] = -amp43 * (1. - 3.*eta) * an->si * ( 4.*an->sHi6*cos(4.*Psi-3.*alpha) - 4.*an->cHi6*cos(4.*Psi+3.*alpha) - an->sHi4*(an->ci+0.5)/2.*cos(2.*Psi-3.*alpha) + an->cHi4*(an->ci-0.5)*cos(2.*Psi+3.*alpha) ); /****/

  hL4->data[-3+os]+= amp43*I*(1. - 3.*eta) * an->si * ( 4.*an->sHi6*sin(4.*Psi-3.*alpha) + 4.*an->cHi6*sin(4.*Psi+3.*alpha) - an->sHi4*(an->ci+0.5)/2.*sin(2.*Psi-3.*alpha) + an->cHi4*(an->ci-0.5)*sin(2.*Psi+3.*alpha) ); /****/

  hL4->data[2+os] = amp42 * (1. - 3.*eta) * ( 16.*an->sHi6*an->cHi2*cos(4.*Psi-2.*alpha) + 16.*an->cHi6*an->sHi2*cos(4.*Psi+2.*alpha) - an->cHi4*cos(2.*(Psi+alpha))*(an->cDi-2.*an->ci+9./7.)/2. - an->sHi4*cos(2.*(Psi-alpha))*(an->cDi+2.*an->ci+9./7.)/2. );

  hL4->data[2+os]+= amp42 *I*(1. - 3.*eta) * ( 16.*an->sHi6*an->cHi2 * sin(4.*Psi-2.*alpha) - 16.*an->cHi6*an->sHi2*sin(4.*Psi+2.*alpha) + an->cHi4*sin(2.*(Psi+alpha))*(an->cDi-2.*an->ci+9./7.)/2. - an->sHi4*sin(2.*(Psi-alpha))*(an->cDi+2.*an->ci+9./7.)/2. );

  hL4->data[-2+os] = amp42 * (1. - 3.*eta) * ( 16.*an->sHi6*an->cHi2*cos(4.*Psi-2.*alpha) + 16.*an->cHi6*an->sHi2*cos(4.*Psi+2.*alpha) - an->cHi4*cos(2.*(Psi+alpha))*(an->cDi-2.*an->ci+9./7.)/2. - an->sHi4*cos(2.*(Psi-alpha))*(an->cDi+2.*an->ci+9./7.)/2. );

  hL4->data[-2+os]+=-amp42 *I*(1. - 3.*eta) * ( 16.*an->sHi6*an->cHi2*sin(4.*Psi-2.*alpha) - 16.*an->cHi6*an->sHi2*sin(4.*Psi+2.*alpha) + an->cHi4*sin(2.*(Psi+alpha))*(an->cDi-2.*an->ci+9./7.)/2. - an->sHi4*sin(2.*(Psi-alpha))*(an->cDi+2.*an->ci+9./7.)/2. );

  hL4->data[1+os] = amp41 * (1. - 3.*eta) * ( -64.*an->sHi5*an->cHi3*cos(4.*Psi-alpha) + 64.*an->sHi3*an->cHi5*cos(4.*Psi+alpha) - an->sHi3*cos(2.*Psi-alpha)*((an->cDi*an->cHi-an->sDi*an->sHi) + 2.*(an->cHi*an->ci-an->sHi*an->si) + 19./7.*an->cHi) + an->cHi3*cos(2.*Psi+alpha)*((an->cDi*an->sHi+an->sDi*an->cHi) - 2.*(an->si*an->cHi+an->ci*an->si2) +19./7.*an->cHi) );

  hL4->data[1+os]+= amp41*I*(1. - 3.*eta) * ( -64.*an->sHi5*an->cHi3 * sin(4.*Psi-alpha) - 64.*an->sHi3*an->cHi5 * sin(4.*Psi+alpha) - an->sHi3*sin(2.*Psi-alpha)*((an->cDi*an->cHi-an->sDi*an->sHi) + 2.*(an->cHi*an->ci-an->sHi*an->si) + 19./7.*an->cHi) - an->cHi3*sin(2.*Psi+alpha)*((an->cDi*an->sHi+an->sDi*an->cHi) - 2.*(an->si*an->cHi+an->ci*an->sHi) + 19./7.*an->cHi) );

  hL4->data[-1+os] = -amp41 * (1. - 3.*eta) * ( -64*an->sHi5*an->cHi3 * cos(4.*Psi-alpha) + 64.*an->sHi3*an->cHi5*cos(4.*Psi+alpha) - an->sHi3*cos(2.*Psi-alpha)*((an->cDi*an->cHi-an->sDi*an->sHi) + 2.*(an->cHi*an->ci-an->sHi*an->si) + 19./7.*an->ci2) + an->cHi3*cos(2.*Psi+alpha)*((an->cDi*an->sHi+an->sDi*an->cHi) - 2.*(an->si*an->cHi+an->ci*an->sHi) + 19./7.*an->ci2) );

  hL4->data[-1+os]+= amp41 *I*(1. - 3.*eta) *I*( -64.*an->sHi5*an->cHi3 * sin(4.*Psi-alpha) - 64.*an->sHi3*an->cHi5 * sin(4.*Psi+alpha) - an->sHi3*sin(2.*Psi-alpha)*((an->cDi*an->cHi-an->sDi*an->sHi) + 2.*(an->cHi*an->ci-an->sHi*an->si) + 19./7.*an->ci2) - an->cHi3*sin(2.*Psi+alpha)*((an->cDi*an->sHi+an->sDi*an->cHi) - 2.*(an->si*an->cHi+an->ci*an->sHi) + 19./7.*an->cHi) );

  hL4->data[os] = amp40 * (1.-3.*eta) * an->si2 * (8.*an->si2*cos(4.*Psi) + cos(2.*Psi)*(an->cDi+5./7.) );
	
  return XLAL_SUCCESS;
} /* End of XLALSimSpinInspiralFillL4Modes*/

static int XLALSimInspiralSpinTaylorT4Engine(REAL8TimeSeries **omega,      /**< post-Newtonian parameter [returned]*/
					     REAL8TimeSeries **Phi,        /**< orbital phase            [returned]*/
					     REAL8TimeSeries **LNhatx,	   /**< LNhat vector x component [returned]*/
					     REAL8TimeSeries **LNhaty,	   /**< "    "    "  y component [returned]*/
					     REAL8TimeSeries **LNhatz,	   /**< "    "    "  z component [returned]*/
					     REAL8TimeSeries **S1x,	   /**< Spin1 vector x component [returned]*/
					     REAL8TimeSeries **S1y,	   /**< "    "    "  y component [returned]*/
					     REAL8TimeSeries **S1z,	   /**< "    "    "  z component [returned]*/
					     REAL8TimeSeries **S2x,	   /**< Spin2 vector x component [returned]*/
					     REAL8TimeSeries **S2y,	   /**< "    "    "  y component [returned]*/
					     REAL8TimeSeries **S2z,	   /**< "    "    "  z component [returned]*/
					     REAL8TimeSeries **Energy,     /**< Energy                   [returned]*/
					     const REAL8 yinit[],
					     const INT4  lengthH,
					     const Approximant approx,     /** Allow to choose w/o ringdown */
					     LALSimInspiralSpinTaylorT4Coeffs *params
					     )
{
  UINT4 idx;
  int jdx;
  UINT4 intLen;
  int intReturn;

  REAL8 S1x0,S1y0,S1z0,S2x0,S2y0,S2z0;  /** Used to store initial spin values */
  REAL8Array *yout;	                /** Used to store integration output */

  ark4GSLIntegrator *integrator;

  /* allocate the integrator */
  if (approx == PhenSpinTaylor)
    integrator = XLALAdaptiveRungeKutta4Init(LAL_NUM_PST4_VARIABLES,XLALSpinInspiralDerivatives,XLALSimSpinInspiralTest,LAL_PST4_ABSOLUTE_TOLERANCE,LAL_PST4_RELATIVE_TOLERANCE);
  else
    integrator = XLALAdaptiveRungeKutta4Init(LAL_NUM_PST4_VARIABLES,XLALSpinInspiralDerivatives,XLALSimIMRPhenSpinTest,LAL_PST4_ABSOLUTE_TOLERANCE,LAL_PST4_RELATIVE_TOLERANCE);

  if (!integrator) {
    XLALPrintError("XLAL Error - %s: Cannot allocate integrator\n", __func__);
    XLAL_ERROR(XLAL_EFUNC);
  }

  /* stop the integration only when the test is true */
  integrator->stopontestonly = 1;

  REAL8 *yin = (REAL8 *) LALMalloc(sizeof(REAL8) * LAL_NUM_PST4_VARIABLES);
  for (idx=0; idx<LAL_NUM_PST4_VARIABLES; idx++) yin[idx]=yinit[idx];
  S1x0=yinit[5];
  S1y0=yinit[6];
  S1z0=yinit[7];
  S2x0=yinit[8];
  S2y0=yinit[9];
  S2z0=yinit[10];

  REAL8 length=((REAL8)lengthH)*fabs(params->dt)/params->M;
  //REAL8 dtInt=1./params->fStart/50.*fabs(params->dt)/params->dt;
  intLen    = XLALAdaptiveRungeKutta4Hermite(integrator,(void *)params,yin,0.0,length,params->dt/params->M,&yout);
  intReturn = integrator->returncode;
  XLALAdaptiveRungeKutta4Free(integrator);

  if (intReturn == XLAL_FAILURE) {
    XLALPrintError("** LALSimIMRPSpinInspiralRD Error **: Adaptive Integrator\n");
    XLALPrintError("             m:  %12.4e  %12.4e  Mom  %12.4e\n",params->m1ByM*params->M,params->m2ByM*params->M,params->fStart);
    XLALPrintError("             S1: %12.4e  %12.4e  %12.4e\n",S1x0,S1y0,S1z0);
    XLALPrintError("             S2: %12.4e  %12.4e  %12.4e\n",S2x0,S2y0,S2z0);
    XLAL_ERROR(XLAL_EFUNC);
  }
  /* End integration*/

  /* Start of the integration checks*/
  if (intLen<minIntLen) {
    XLALPrintError("** LALSimIMRPSpinInspiralRD ERROR **: integration too short! intReturnCode %d, integration length %d, at least %d required\n",intReturn,intLen,minIntLen);
    if (XLALClearErrno() == XLAL_ENOMEM) {
      XLAL_ERROR(  XLAL_ENOMEM);
    } else {
      XLAL_ERROR( XLAL_EFAILED);
    }
  }

  const LIGOTimeGPS tStart=LIGOTIMEGPSZERO;
  *omega  = XLALCreateREAL8TimeSeries( "OMEGA", &tStart, 0., params->dt, &lalDimensionlessUnit, intLen);
  *Phi    = XLALCreateREAL8TimeSeries( "ORBITAL_PHASE", &tStart, 0., params->dt, &lalDimensionlessUnit, intLen); 
  *LNhatx = XLALCreateREAL8TimeSeries( "LNHAT_X_COMPONENT", &tStart, 0., params->dt, &lalDimensionlessUnit, intLen); 
  *LNhaty = XLALCreateREAL8TimeSeries( "LNHAT_Y_COMPONENT", &tStart, 0., params->dt, &lalDimensionlessUnit, intLen);
  *LNhatz = XLALCreateREAL8TimeSeries( "LNHAT_Z_COMPONENT", &tStart, 0., params->dt, &lalDimensionlessUnit, intLen);
  *S1x    = XLALCreateREAL8TimeSeries( "SPIN1_X_COMPONENT", &tStart, 0., params->dt, &lalDimensionlessUnit, intLen);
  *S1y    = XLALCreateREAL8TimeSeries( "SPIN1_Y_COMPONENT", &tStart, 0., params->dt, &lalDimensionlessUnit, intLen);
  *S1z    = XLALCreateREAL8TimeSeries( "SPIN1_Z_COMPONENT", &tStart, 0., params->dt, &lalDimensionlessUnit, intLen);
  *S2x    = XLALCreateREAL8TimeSeries( "SPIN2_X_COMPONENT", &tStart, 0., params->dt, &lalDimensionlessUnit, intLen);
  *S2y    = XLALCreateREAL8TimeSeries( "SPIN2_Y_COMPONENT", &tStart, 0., params->dt, &lalDimensionlessUnit, intLen); 
  *S2z    = XLALCreateREAL8TimeSeries( "SPIN2_Z_COMPONENT", &tStart, 0., params->dt, &lalDimensionlessUnit, intLen);
  *Energy = XLALCreateREAL8TimeSeries( "LNHAT_Z_COMPONENT", &tStart, 0., params->dt, &lalDimensionlessUnit, intLen);
  if ( !omega || !Phi || !S1x || !S1y || !S1z || !S2x || !S2y || !S2z || !LNhatx || !LNhaty || !LNhatz || !Energy ) {
    XLALDestroyREAL8Array(yout);
    XLAL_ERROR(XLAL_EFUNC);
  }

  /* Copy dynamical variables from yout array to output time series.
   * Note the first 'len' members of yout are the time steps. 
   */
  int sign=params->dt > 0. ? 1 : -1;
  jdx= (intLen-1)*(-sign+1)/2;

  for (idx=0;idx<intLen;idx++) {
    (*Phi)->data->data[idx]    = yout->data[intLen+jdx];
    (*omega)->data->data[idx]  = yout->data[2*intLen+jdx];
    (*LNhatx)->data->data[idx] = yout->data[3*intLen+jdx];
    (*LNhaty)->data->data[idx] = yout->data[4*intLen+jdx];
    (*LNhatz)->data->data[idx] = yout->data[5*intLen+jdx];
    (*S1x)->data->data[idx]    = yout->data[6*intLen+jdx];
    (*S1y)->data->data[idx]    = yout->data[7*intLen+jdx];
    (*S1z)->data->data[idx]    = yout->data[8*intLen+jdx];
    (*S2x)->data->data[idx]    = yout->data[9*intLen+jdx];
    (*S2y)->data->data[idx]    = yout->data[10*intLen+jdx];
    (*S2z)->data->data[idx]    = yout->data[11*intLen+jdx];
    (*Energy)->data->data[idx] = yout->data[12*intLen+jdx];
    jdx+=sign;
  }

  XLALDestroyREAL8Array(yout);
  return intReturn;
} /* End of XLALSimInspiralSpinTaylorT4Engine */

int XLALSimInspiralComputeInclAngle(REAL8 ciota, LALSimInspiralInclAngle *angle){
  angle->ci=ciota;
  angle->si=sqrt(1.-ciota*ciota);
  angle->ci2=angle->ci*angle->ci;
  angle->si2=angle->si*angle->si;
  angle->cDi=angle->ci*angle->ci-angle->si*angle->si;
  angle->sDi=2.*angle->ci*angle->si;
  angle->cHi=sqrt((1.+angle->ci)/2.);
  angle->sHi=sqrt((1.-angle->ci)/2.);
  angle->cHi2=(1.+angle->ci)/2.;
  angle->sHi2=(1.-angle->ci)/2.;
  angle->cHi3=angle->cHi*angle->cHi2;
  angle->sHi3=angle->sHi*angle->sHi2;
  angle->cHi4=angle->cHi2*angle->cHi2;
  angle->sHi4=angle->sHi2*angle->sHi2;
  angle->cHi6=angle->cHi2*angle->cHi4;
  angle->sHi6=angle->sHi2*angle->sHi4;
  angle->cHi8=angle->cHi4*angle->cHi4;
  angle->sHi8=angle->sHi4*angle->sHi4;

  return XLAL_SUCCESS;

} /* End of XLALSimInspiralComputeInclAngle*/

/** The following lines are necessary in the case L is initially parallel to 
 * N so that alpha is undefined at the beginning but different from zero at the first 
 * step (this happens if the spins are not aligned with L). 
 * Such a discontinuity of alpha would induce 
 * a discontinuity of the waveform between its initial value and its value after the
 * first integration step. This does not happen during the integration as in that 
 * case alpha can be safely set to the previous value, just before L becomes parallel 
 * to N. In the case L stays all the time parallel to N than alpha can be 
 * safely set to zero, as it is.
 **/
 
static int XLALSimInspiralComputeAlpha(LALSimInspiralSpinTaylorT4Coeffs params, REAL8 LNhx, REAL8 LNhy, REAL8 S1x, REAL8 S1y, REAL8 S2x, REAL8 S2y,REAL8 *alpha){
  if ((LNhy*LNhy+LNhx*LNhx)==0.) {
    REAL8 S1xy=S1x*S1x+S1y*S1y;
    REAL8 S2xy=S2x*S2x+S2y*S2y;
    if ((S1xy+S2xy)==0.) {
      *alpha=0.;
    }
    else {
      REAL8 c1=0.75+params.eta/2-0.75*params.dmByM;
      REAL8 c2=0.75+params.eta/2+0.75*params.dmByM;
      *alpha=atan2(-c1*S1x-c2*S2x,c1*S1y+c2*S2y);
    }
  }
  else {
    *alpha=atan2(LNhy,LNhx);
  }
  return XLAL_SUCCESS;
} /*End of XLALSimInspiralComputeAlpha*/

/** Here we use the following convention:
 *  the coordinates of the spin vectors spin1,2 and the inclination 
 *  variable refers to different physical parameters according to the value of 
 *  axisChoice:
 *  * LAL_SIM_INSPIRAL_FRAME_AXIS_ORBITAL_L: inclination denotes the angle 
 *            between the view direction N and the initial L 
 *            (initial L//z, N in the x-z plane) and the spin 
 * 	      coordinates are given with respect to initial L.
 *  * LAL_SIM_INSPIRAL_FRAME_AXIS_TOTAL_J:   inclination denotes the angle 
 *            between the view direction and J (J is constant during the 
 *            evolution, J//z, both N and initial L are in the x-z plane) 
 *            and the spin coordinates are given wrt initial L.
 *  * LAL_SIM_INSPIRAL_FRAME_AXIS_VIEW:     inclination denotes the angle 
 *            between the initial L and N (N//z, initial L in the x-z plane)
 *            and the spin coordinates are given with respect to N.
 *
 *   In order to reproduce the results of the SpinTaylor code View must be chosen.
 *   The spin magnitude are normalized to the individual mass^2, i.e.
 *   they are dimension-less.
 *   The modulus of the initial angular momentum is fixed by m1,m2 and
 *   initial frequency.
 *   The polarization angle is not used here, it enters the pattern
 *   functions along with the angles marking the sky position of the
 *   source.
 **/

/*static void rotateX(REAL8 phi,REAL8 *vx, REAL8 *vy, REAL8 *vz){
  REAL8 tmp[3]={*vx,*vy,*vz};
  *vx=*vy=*vz=0.;
  REAL8 rotX[3][3]={{1.,0.,0.},{0,cos(phi),-sin(phi)},{0,sin(phi),cos(phi)}};
  int idx;
  for (idx=0;idx<3;idx++) {
    *vx+=rotX[0][idx]*tmp[idx];
    *vy+=rotX[1][idx]*tmp[idx];
    *vz+=rotX[2][idx]*tmp[idx];
  }
  }*/
static void rotateY(REAL8 phi,REAL8 *vx, REAL8 *vy, REAL8 *vz){
  REAL8 rotY[3][3]={{cos(phi),0.,sin(phi)},{0.,1.,0.},{-sin(phi),0.,cos(phi)}};
  REAL8 tmp[3]={*vx,*vy,*vz};
  *vx=*vy=*vz=0.;
  int idx;
  for (idx=0;idx<3;idx++) {
    *vx+=rotY[0][idx]*tmp[idx];
    *vy+=rotY[1][idx]*tmp[idx];
    *vz+=rotY[2][idx]*tmp[idx];
  }
}
static void rotateZ(REAL8 phi,REAL8 *vx, REAL8 *vy, REAL8 *vz){
  REAL8 tmp[3]={*vx,*vy,*vz};
  REAL8 rotZ[3][3]={{cos(phi),-sin(phi),0.},{sin(phi),cos(phi),0.},{0.,0.,1.}};
  *vx=*vy=*vz=0.;
  int idx;
  for (idx=0;idx<3;idx++) {
    *vx+=rotZ[0][idx]*tmp[idx];
    *vy+=rotZ[1][idx]*tmp[idx];
    *vz+=rotZ[2][idx]*tmp[idx];
  }
}

static int XLALSimIMRPhenSpinInspiralSetAxis(REAL8 mass1, /* in MSun units */
					     REAL8 mass2, /* in MSun units */
					     REAL8 *iota, /* input/output */
					     REAL8 *yinit,/* RETURNED */
					     LALSimInspiralFrameAxis axisChoice)
{
  // Magnitude of the Newtonian orbital angular momentum
  REAL8 omega=yinit[1];
  REAL8 Mass=mass1+mass2;
  REAL8 Lmag = mass1*mass2 / cbrt(omega);
  REAL8 Jmag;
  REAL8 S1[3],S2[3],J[3],LNh[3],N[3];
  REAL8 inc;
  REAL8 phiJ,thetaJ,phiN;

  // Physical values of the spins
  inc=*iota;
  S1[0] =  yinit[5] * mass1 * mass1;
  S1[1] =  yinit[6] * mass1 * mass1;
  S1[2] =  yinit[7] * mass1 * mass1;
  S2[0] =  yinit[8] * mass2 * mass2;
  S2[1] =  yinit[9] * mass2 * mass2;
  S2[2] = yinit[10] * mass2 * mass2;

  switch (axisChoice) {

  case LAL_SIM_INSPIRAL_FRAME_AXIS_ORBITAL_L:
    J[0]=S1[0]+S2[0];
    J[1]=S1[1]+S2[1];
    J[2]=S1[2]+S2[2]+Lmag;
    N[0]=sin(inc);
    N[1]=0.;
    N[2]=cos(inc);
    LNh[0]=0.;
    LNh[1]=0.;
    LNh[2]=1.;
    Jmag=sqrt(J[0]*J[0]+J[1]*J[1]+J[2]*J[2]);
    if (Jmag>0.) phiJ=atan2(J[1],J[0]);
    else phiJ=0.;
    thetaJ=acos(J[2]/Jmag);
    rotateZ(-phiJ,&N[0],&N[1],&N[2]);
    rotateY(-thetaJ,&N[0],&N[1],&N[2]);
    break;

  case LAL_SIM_INSPIRAL_FRAME_AXIS_TOTAL_J:
    J[0]=S1[0]+S2[0];
    J[1]=S1[1]+S2[1];
    J[2]=S1[2]+S2[2]+Lmag;
    LNh[0]=0.;
    LNh[1]=0.;
    LNh[2]=1.;
    N[0]=sin(inc);
    N[1]=0.;
    N[2]=cos(inc);
    Jmag=sqrt(J[0]*J[0]+J[1]*J[1]+J[2]*J[2]);
    if (Jmag>0.) phiJ=atan2(J[1],J[0]);
    else phiJ=0.;
    thetaJ=acos(J[2]/Jmag);
    break;

  case LAL_SIM_INSPIRAL_FRAME_AXIS_VIEW:
  default:
    LNh[0] = sin(inc);
    LNh[1] = 0.;
    LNh[2] = cos(inc);
    J[0]=S1[0]+S2[0]+LNh[0]*Lmag;
    J[1]=S1[1]+S2[1]+LNh[1]*Lmag;
    J[2]=S1[2]+S2[2]+LNh[2]*Lmag;
    N[0]=0.;
    N[1]=0.;
    N[2]=1.;
    Jmag=sqrt(J[0]*J[0]+J[1]*J[1]+J[2]*J[2]);
    if (Jmag>0.) phiJ=atan2(J[1],J[0]);
    else phiJ=0.;
    thetaJ=acos(J[2]/Jmag);
    rotateZ(-phiJ,&N[0],&N[1],&N[2]);
    rotateY(-thetaJ,&N[0],&N[1],&N[2]);
    break;
  }

  rotateZ(-phiJ,&S1[0],&S1[1],&S1[2]);
  rotateZ(-phiJ,&S2[0],&S2[1],&S2[2]);
  rotateZ(-phiJ,&LNh[0],&LNh[1],&LNh[2]);
  rotateY(-thetaJ,&S1[0],&S1[1],&S1[2]);
  rotateY(-thetaJ,&S2[0],&S2[1],&S2[2]);
  rotateY(-thetaJ,&LNh[0],&LNh[1],&LNh[2]);
  phiN=atan2(N[1],N[0]);
  rotateZ(-phiN,&S1[0],&S1[1],&S1[2]);
  rotateZ(-phiN,&S2[0],&S2[1],&S2[2]);
  rotateZ(-phiN,&LNh[0],&LNh[1],&LNh[2]);
  rotateZ(-phiN,&N[0],&N[1],&N[2]);
  inc = acos(N[2]);
  *iota=inc;
  yinit[2] = LNh[0];
  yinit[3] = LNh[1];
  yinit[4] = LNh[2];
  yinit[5] = S1[0]/Mass/Mass;
  yinit[6] = S1[1]/Mass/Mass;
  yinit[7] = S1[2]/Mass/Mass;
  yinit[8] = S2[0]/Mass/Mass;
  yinit[9] = S2[1]/Mass/Mass;
  yinit[10]= S2[2]/Mass/Mass;

  return XLAL_SUCCESS;

} /* End of XLALSimIMRPhenSpinInspiralSetAxis*/

/**
 * PhenSpin Initialization
 **/

static int XLALSimIMRPhenSpinInitialize(REAL8 mass1,                              /* in Msun units */
					REAL8 mass2,                              /* in Msun units */
					REAL8 *yinit,
					REAL8 fStart,                             /* in Hz*/
					REAL8 fEnd,                               /* in Hz*/
					REAL8 deltaT,
					int phaseO,
					LALSimInspiralSpinTaylorT4Coeffs *params,
					LALSimInspiralWaveformFlags      *waveFlags,
					LALSimInspiralTestGRParam        *testGRparams,
					Approximant approx)
{
  if (fStart<=0.) {
    XLALPrintError("** LALSimIMRPSpinInspiralRD error *** non >ve value of fMin %12.4e\n",fStart);
    XLAL_ERROR(XLAL_EDOM);
  }

  REAL8 S1x=yinit[5];
  REAL8 S1y=yinit[6];
  REAL8 S1z=yinit[7];
  REAL8 S2x=yinit[8];
  REAL8 S2y=yinit[9];
  REAL8 S2z=yinit[10];

  REAL8 LNhS1 = S1z;
  REAL8 LNhS2 = S2z;
  REAL8 S1S1  = S1x*S1x + S1y*S1y + S1z*S1z;
  REAL8 S1S2  = S1x*S2x + S1y*S2y + S1z*S2z;
  REAL8 S2S2  = S2x*S2x + S2y*S2y + S2z*S2z;
  REAL8 unitHz     = (mass1+mass2)*LAL_MTSUN_SI; /* convert m from msun to seconds */
  REAL8 initOmega  = fStart*unitHz * (REAL8) LAL_PI;
  REAL8 omegaMatch = OmMatch(LNhS1,LNhS2,S1S1,S1S2,S2S2);
  yinit[1]=initOmega;

  if (approx==PhenSpinTaylorRD) {
    if ( initOmega > omegaMatch ) {
      if ((S1x==S1y)&&(S1x==0)&&(S2x==S2y)&&(S2y==0.)) {
	initOmega = 0.95*omegaMatch;
	yinit[1]=initOmega;
	XLALPrintWarning("*** LALSimIMRPSpinInspiralRD WARNING ***: Initial frequency reset from %12.6e to %12.6e Hz, m:(%12.4e,%12.4e)\n",fStart,initOmega/unitHz/LAL_PI,mass1,mass2);
      }
      else {
	XLALPrintError("*** LALSimIMRPSpinInspiralRD ERROR ***: Initial frequency %12.6e Hz too high, as fMatch estimated %12.6e Hz, m:(%12.4e,%12.4e)\n",fStart,omegaMatch/unitHz/LAL_PI,mass1,mass2);
	XLAL_ERROR(XLAL_EFAILED);
      }
    }
  }

  /* setup coefficients for PN equations */
  if(XLALSimIMRPhenSpinParamsSetup(params,deltaT,fStart,fEnd,mass1,mass2,XLALSimInspiralGetInteraction(waveFlags),testGRparams,phaseO)) {
    XLAL_ERROR(XLAL_ENOMEM);
  }

  return XLAL_SUCCESS;

} /* End of XLALSimIMRPhenSpinInitialize*/

/* Appends the start and end time series together, skipping the redundant last
 * sample of begin.  Frees end before returning a pointer to the result, which is
 * the resized start series.  */
static REAL8TimeSeries *appendTSandFree(REAL8TimeSeries *start, REAL8TimeSeries *end) {
    unsigned int origlen = start->data->length;
    start = XLALResizeREAL8TimeSeries(start, 0, 
            start->data->length + end->data->length - 1);
    
    memcpy(start->data->data + origlen -2, end->data->data, 
            (end->data->length)*sizeof(REAL8));

    XLALGPSAdd(&(start->epoch), -end->deltaT*(end->data->length - 1));

    XLALDestroyREAL8TimeSeries(end);

    return start;        
}

/**
 * Driver routine to compute the PhenSpin Inspiral waveform
 * without ring-down
 *
 * All units are SI units.
 */
int XLALSimSpinInspiralGenerator(REAL8TimeSeries **hPlus,	        /**< +-polarization waveform [returned] */
				 REAL8TimeSeries **hCross,	        /**< x-polarization waveform [returned] */
				 REAL8 phi_start,                       /**< start phase */
				 REAL8 deltaT,                          /**< sampling interval */
				 REAL8 m1,                              /**< mass of companion 1 */
				 REAL8 m2,                              /**< mass of companion 2 */
				 REAL8 f_start,                         /**< start frequency */
				 REAL8 f_ref,                           /**< reference frequency */
				 REAL8 r,                               /**< distance of source */
				 REAL8 iota,                            /**< incination of source (rad) */
				 REAL8 s1x,                             /**< x-component of dimensionless spin for object 1 */
				 REAL8 s1y,                             /**< y-component of dimensionless spin for object 1 */
				 REAL8 s1z,                             /**< z-component of dimensionless spin for object 1 */
				 REAL8 s2x,                             /**< x-component of dimensionless spin for object 2 */
				 REAL8 s2y,                             /**< y-component of dimensionless spin for object 2 */
				 REAL8 s2z,                             /**< z-component of dimensionless spin for object 2 */
				 int phaseO,                            /**< twice post-Newtonian phase order */
				 int UNUSED ampO,                       /**< twice post-Newtonian amplitude order */
				 LALSimInspiralWaveformFlags *waveFlags,/**< Choice of axis for input spin params */
				 LALSimInspiralTestGRParam *testGRparams/**< Non-GR params */
				 )
{

  int errcode=0;
  int errcodeInt=0;
  int intLen;         /* Length of arrays after integration*/
  int lengthH;
  int idx,kdx;
  LALSimInspiralSpinTaylorT4Coeffs params;
  REAL8 mass1=m1/LAL_MSUN_SI;
  REAL8 mass2=m2/LAL_MSUN_SI;

  REAL8 yinit[LAL_NUM_PST4_VARIABLES];
  yinit[0] = phi_start;
  yinit[1] = 0.;
  yinit[2] = 0.;
  yinit[3] = 0.;
  yinit[4] = cos(iota);
  yinit[5] = s1x;
  yinit[6] = s1y;
  yinit[7] = s1z;
  yinit[8] = s2x;
  yinit[9] = s2y;
  yinit[10]= s2z;
  yinit[11]= 0.;

  REAL8 tn = XLALSimInspiralTaylorLength(deltaT, m1, m2, f_start, phaseO);
  REAL8 x  = 1.1 * (tn + 1. ) / deltaT;
  int length = ceil(log10(x)/log10(2.));
  lengthH    = pow(2, length);
  REAL8TimeSeries *omega=NULL;
  REAL8TimeSeries *Phi=NULL;
  REAL8TimeSeries *LNhatx=NULL;
  REAL8TimeSeries *LNhaty=NULL;
  REAL8TimeSeries *LNhatz=NULL;
  REAL8TimeSeries *S1x=NULL;
  REAL8TimeSeries *S1y=NULL;
  REAL8TimeSeries *S1z=NULL;
  REAL8TimeSeries *S2x=NULL;
  REAL8TimeSeries *S2y=NULL;
  REAL8TimeSeries *S2z=NULL;
  REAL8TimeSeries *Energy=NULL;

  if (f_ref<=f_start) {
    errcode=XLALSimIMRPhenSpinInitialize(mass1,mass2,yinit,f_start,-1.,deltaT,phaseO,&params,waveFlags,testGRparams,XLALGetApproximantFromString("PhenSpinTaylor"));
    if(errcode) XLAL_ERROR(XLAL_EFUNC);
    if(XLALSimIMRPhenSpinInspiralSetAxis(mass1,mass2,&iota,yinit,XLALSimInspiralGetFrameAxis(waveFlags))) {
      XLAL_ERROR(XLAL_EFUNC);
    }
    errcodeInt=XLALSimInspiralSpinTaylorT4Engine(&omega,&Phi,&LNhatx,&LNhaty,&LNhatz,&S1x,&S1y,&S1z,&S2x,&S2y,&S2z,&Energy,yinit,lengthH,PhenSpinTaylor,&params);
    intLen=Phi->data->length;
  }
  else {
    REAL8TimeSeries *Phi1,*omega1,*LNhatx1,*LNhaty1,*LNhatz1,*S1x1,*S1y1,*S1z1,*S2x1,*S2y1,*S2z1,*Energy1;
    errcode=XLALSimIMRPhenSpinInitialize(mass1,mass2,yinit,f_ref,f_start,deltaT,phaseO,&params,waveFlags,testGRparams,XLALGetApproximantFromString("PhenSpinTaylor"));
    if(errcode) XLAL_ERROR(XLAL_EFUNC);
    if(XLALSimIMRPhenSpinInspiralSetAxis(mass1,mass2,&iota,yinit,XLALSimInspiralGetFrameAxis(waveFlags))) {
      XLAL_ERROR(XLAL_EFUNC);
    }

    REAL8 dyTmp[LAL_NUM_PST4_VARIABLES];
    REAL8 energy;
    XLALSpinInspiralDerivatives(0., yinit,dyTmp,&params);
    energy=dyTmp[11]*params.dt/params.M+yinit[11];
    yinit[11]=energy;

    errcodeInt=XLALSimInspiralSpinTaylorT4Engine(&omega1,&Phi1,&LNhatx1,&LNhaty1,&LNhatz1,&S1x1,&S1y1,&S1z1,&S2x1,&S2y1,&S2z1,&Energy1,yinit,lengthH,PhenSpinTaylor,&params);

    int intLen1=Phi1->data->length;
    /* report on abnormal termination*/
    if ( (errcodeInt != LALSIMINSPIRAL_PHENSPIN_TEST_FREQBOUND) ) {
      XLALPrintError("** LALPSpinInspiralRD WARNING **: integration terminated with code %d.\n",errcode);
      XLALPrintError("   1025: Energy increases\n  1026: Omegadot -ve\n  1027: Freqbound\n 1028: Omega NAN\n  1031: Omega -ve\n");
      XLALPrintError("  Waveform parameters were m1 = %14.6e, m2 = %14.6e, inc = %10.6f,  fref %10.4f Hz\n", m1, m2, iota, f_ref);
      XLALPrintError("                           S1 = (%10.6f,%10.6f,%10.6f)\n", s1x, s1y, s1z);
      XLALPrintError("                           S2 = (%10.6f,%10.6f,%10.6f)\n", s2x, s2y, s2z);
    }

    yinit[0] = Phi1->data->data[intLen1-1];
    yinit[1] = omega1->data->data[intLen1-1];
    yinit[2] = LNhatx1->data->data[intLen1-1];
    yinit[3] = LNhaty1->data->data[intLen1-1];
    yinit[4] = LNhatz1->data->data[intLen1-1];
    yinit[5] = S1x1->data->data[intLen1-1];
    yinit[6] = S1y1->data->data[intLen1-1];
    yinit[7] = S1z1->data->data[intLen1-1];
    yinit[8] = S2x1->data->data[intLen1-1];
    yinit[9] = S2y1->data->data[intLen1-1];
    yinit[10]= S2z1->data->data[intLen1-1];
    yinit[11]= Energy1->data->data[intLen1-1];

    REAL8TimeSeries *omega2,*Phi2,*LNhatx2,*LNhaty2,*LNhatz2,*S1x2,*S1y2,*S1z2,*S2x2,*S2y2,*S2z2,*Energy2;

    params.fEnd=-1.;
    params.dt*=-1.;
    errcodeInt=XLALSimInspiralSpinTaylorT4Engine(&omega2,&Phi2,&LNhatx2,&LNhaty2,&LNhatz2,&S1x2,&S1y2,&S1z2,&S2x2,&S2y2,&S2z2,&Energy2,yinit,lengthH,PhenSpinTaylor,&params);

    REAL8 phiRef=Phi1->data->data[Phi1->data->length-1];

    omega =appendTSandFree(omega1,omega2);
    Phi   =appendTSandFree(Phi1,Phi2);
    LNhatx=appendTSandFree(LNhatx1,LNhatx2);
    LNhaty=appendTSandFree(LNhaty1,LNhaty2);
    LNhatz=appendTSandFree(LNhatz1,LNhatz2);
    S1x   =appendTSandFree(S1x1,S1x2);
    S1y   =appendTSandFree(S1y1,S1y2);
    S1z   =appendTSandFree(S1z1,S1z2);
    S2x   =appendTSandFree(S2x1,S2x2);
    S2y   =appendTSandFree(S2y1,S2y2);
    S2z   =appendTSandFree(S2z1,S2z2);
    Energy=appendTSandFree(Energy1,Energy2);
    intLen=Phi->data->length;
    for (idx=0;idx<intLen;idx++) Phi->data->data[idx]-=phiRef;

  }

  /* report on abnormal termination*/
  if ( (errcodeInt !=  LALSIMINSPIRAL_PHENSPIN_TEST_ENERGY) ) {
    XLALPrintWarning("** LALPSpinInspiralRD WARNING **: integration terminated with code %d.\n",errcode);
    XLALPrintWarning("  Waveform parameters were m1 = %14.6e, m2 = %14.6e, inc = %10.6f,\n", m1, m2, iota);
    XLALPrintWarning("                           S1 = (%10.6f,%10.6f,%10.6f)\n", s1x, s1y, s1z);
    XLALPrintWarning("                           S2 = (%10.6f,%10.6f,%10.6f)\n", s2x, s2y, s2z);
  }

  LIGOTimeGPS tStart=LIGOTIMEGPSZERO;
  COMPLEX16Vector* hL2tmp=XLALCreateCOMPLEX16Vector(5);
  COMPLEX16Vector* hL3tmp=XLALCreateCOMPLEX16Vector(7);
  COMPLEX16Vector* hL4tmp=XLALCreateCOMPLEX16Vector(9);
  COMPLEX16TimeSeries* hL2=XLALCreateCOMPLEX16TimeSeries( "hL2", &tStart, 0., deltaT, &lalDimensionlessUnit, 5*intLen);
  COMPLEX16TimeSeries* hL3=XLALCreateCOMPLEX16TimeSeries( "hL3", &tStart, 0., deltaT, &lalDimensionlessUnit, 7*intLen);
  COMPLEX16TimeSeries* hL4=XLALCreateCOMPLEX16TimeSeries( "hL4", &tStart, 0., deltaT, &lalDimensionlessUnit, 9*intLen);
  for (idx=0;idx<(int)hL2->data->length;idx++) hL2->data->data[idx]=0.;
  for (idx=0;idx<(int)hL3->data->length;idx++) hL3->data->data[idx]=0.;
  for (idx=0;idx<(int)hL4->data->length;idx++) hL4->data->data[idx]=0.;

  REAL8TimeSeries *hPtmp=XLALCreateREAL8TimeSeries( "hPtmp", &tStart, 0., deltaT, &lalDimensionlessUnit, intLen);
  REAL8TimeSeries *hCtmp=XLALCreateREAL8TimeSeries( "hCtmp", &tStart, 0., deltaT, &lalDimensionlessUnit, intLen);
  COMPLEX16TimeSeries *hLMtmp=XLALCreateCOMPLEX16TimeSeries( "hLMtmp", &tStart, 0., deltaT, &lalDimensionlessUnit, intLen);
  for (idx=0;idx<(int)hPtmp->data->length;idx++) {
    hPtmp->data->data[idx]=0.;
    hCtmp->data->data[idx]=0.;
    hLMtmp->data->data[idx]=0.;
  }

  LALSimInspiralInclAngle trigAngle;

  REAL8 amp22ini = -2.0 * m1*m2/(m1+m2) * LAL_G_SI/pow(LAL_C_SI,2.) / r * sqrt(16. * LAL_PI / 5.);
  REAL8 amp33ini = -amp22ini * sqrt(5./42.)/4.;
  REAL8 amp44ini = amp22ini * sqrt(5./7.) * 2./9.;
  REAL8 alpha,v,v2,Psi,om;
  REAL8 eta=mass1*mass2/(mass1+mass2)/(mass1+mass2);
  REAL8 dm=(mass1-mass2)/(mass1+mass2);

  for (idx=0;idx<intLen;idx++) {
    om=omega->data->data[idx];
    v=cbrt(om);
    v2=v*v;
    Psi=Phi->data->data[idx] -2.*om*(1.-eta*v2)*log(om);
    errcode =XLALSimInspiralComputeAlpha(params,LNhatx->data->data[idx],LNhaty->data->data[idx],S1x->data->data[idx],S1y->data->data[idx],S2x->data->data[idx],S2y->data->data[idx],&alpha);

    errcode+=XLALSimInspiralComputeInclAngle(LNhatz->data->data[idx],&trigAngle);
    errcode+=XLALSimSpinInspiralFillL2Modes(hL2tmp,v,eta,dm,Psi,alpha,&trigAngle);
    for (kdx=0;kdx<5;kdx++) hL2->data->data[5*idx+kdx]=hL2tmp->data[kdx]*amp22ini*v2;
    errcode+=XLALSimSpinInspiralFillL3Modes(hL3tmp,v,eta,dm,Psi,alpha,&trigAngle);
    for (kdx=0;kdx<7;kdx++) hL3->data->data[7*idx+kdx]=hL3tmp->data[kdx]*amp33ini*v2;
    errcode+=XLALSimSpinInspiralFillL4Modes(hL4tmp,v,eta,dm,Psi,alpha,&trigAngle);
    for (kdx=0;kdx<9;kdx++) hL4->data->data[9*idx+kdx]=hL4tmp->data[kdx]*amp44ini*v2*v2;
  }
  XLALDestroyCOMPLEX16Vector(hL2tmp);
  XLALDestroyCOMPLEX16Vector(hL3tmp);
  XLALDestroyCOMPLEX16Vector(hL4tmp);

  XLALDestroyREAL8TimeSeries(omega);
  XLALDestroyREAL8TimeSeries(Phi);
  XLALDestroyREAL8TimeSeries(LNhatx);
  XLALDestroyREAL8TimeSeries(LNhaty);
  XLALDestroyREAL8TimeSeries(LNhatz);
  XLALDestroyREAL8TimeSeries(S1x);
  XLALDestroyREAL8TimeSeries(S1y);
  XLALDestroyREAL8TimeSeries(S1z);
  XLALDestroyREAL8TimeSeries(S2x);
  XLALDestroyREAL8TimeSeries(S2y);
  XLALDestroyREAL8TimeSeries(S2z);
  XLALDestroyREAL8TimeSeries(Energy);

  int m,l;
  LALSimInspiralModesChoice modesChoice=XLALSimInspiralGetModesChoice(waveFlags);
  if ( ( modesChoice &  LAL_SIM_INSPIRAL_MODES_CHOICE_RESTRICTED) ==  LAL_SIM_INSPIRAL_MODES_CHOICE_RESTRICTED ) {
    l=2;
    for (m=-l;m<=l;m++) {
      for (idx=0;idx<intLen;idx++) hLMtmp->data->data[idx]=hL2->data->data[(m+l)+idx*(2*l+1)];
      XLALSimAddMode(hPtmp,hCtmp,hLMtmp,iota,0.,l,m,0);
    }
  }
  XLALDestroyCOMPLEX16TimeSeries(hL2);
  if ( ( modesChoice &  LAL_SIM_INSPIRAL_MODES_CHOICE_3L) ==  LAL_SIM_INSPIRAL_MODES_CHOICE_3L ) {
    l=3;
    for (m=-l;m<=l;m++) {
      for (idx=0;idx<intLen;idx++)
	hLMtmp->data->data[idx]=hL3->data->data[(m+l)+idx*(2*l+1)];
      XLALSimAddMode(hPtmp,hCtmp,hLMtmp,iota,0.,l,m,0);
    }
  }
  XLALDestroyCOMPLEX16TimeSeries(hL3);
  if ( ( modesChoice &  LAL_SIM_INSPIRAL_MODES_CHOICE_3L) ==  LAL_SIM_INSPIRAL_MODES_CHOICE_3L ) {
    l=4;
    for (m=-l;m<=l;m++) {
      for (idx=0;idx<intLen;idx++)
	hLMtmp->data->data[idx]=hL4->data->data[(m+l)+idx*(2*l+1)];
      XLALSimAddMode(hPtmp,hCtmp,hLMtmp,iota,0.,l,m,0);
    }
  }
  XLALDestroyCOMPLEX16TimeSeries(hL4);
  XLALDestroyCOMPLEX16TimeSeries(hLMtmp);

  REAL8 tPeak=intLen*deltaT;
  if ((*hPlus) && (*hCross)) {
    if ((*hPlus)->data->length!=(*hCross)->data->length) {
      XLALPrintError("*** LALSimIMRPSpinInspiralRD ERROR: h+ and hx differ in length: %d vs. %d\n",(*hPlus)->data->length,(*hCross)->data->length);
      XLAL_ERROR(XLAL_EFAILED);
    }
    else {
      if ((int)(*hPlus)->data->length<intLen) {
	XLALPrintError("*** LALSimIMRPSpinInspiralRD ERROR: h+ and hx too short: %d vs. %d\n",(*hPlus)->data->length,intLen);
	XLAL_ERROR(XLAL_EFAILED);
      }
      else {
	XLALGPSAdd(&((*hPlus)->epoch),-tPeak);
	XLALGPSAdd(&((*hCross)->epoch),-tPeak);
      }
    }
  }
  else {
    XLALGPSAdd(&tStart,-tPeak);
    *hPlus  = XLALCreateREAL8TimeSeries("H+", &tStart, 0.0, deltaT, &lalDimensionlessUnit, intLen);
    *hCross = XLALCreateREAL8TimeSeries("Hx", &tStart, 0.0, deltaT, &lalDimensionlessUnit, intLen);
    if(*hPlus == NULL || *hCross == NULL)
      XLAL_ERROR(XLAL_ENOMEM);
  }

  int minLen=hPtmp->data->length < (*hPlus)->data->length ? hPtmp->data->length : (*hPlus)->data->length;
  for (idx=0;idx<minLen;idx++) {
    (*hPlus)->data->data[idx] =hPtmp->data->data[idx];
    (*hCross)->data->data[idx]=hCtmp->data->data[idx];
  }
  for (idx=minLen;idx<(int)(*hPlus)->data->length;idx++) {
    (*hPlus)->data->data[idx] =0.;
    (*hCross)->data->data[idx]=0.;
  }

  XLALDestroyREAL8TimeSeries(hPtmp);
  XLALDestroyREAL8TimeSeries(hCtmp);

  return errcode;

} /* End of XLALSimSpinInspiralGenerator*/

int XLALSimIMRPhenSpinFinalMassSpin(REAL8 *finalMass,
				    REAL8 *finalSpin,
				    REAL8 mass1,
				    REAL8 mass2,
				    REAL8 s1s1,
				    REAL8 s2s2,
				    REAL8 s1L,
				    REAL8 s2L,
				    REAL8 s1s2,
				    REAL8 energy)
{
  /* XLAL error handling */
  INT4 errcode = XLAL_SUCCESS;
  REAL8 qq,ll,eta;
	
  /* See eq.(6) in arXiv:0904.2577 */
  REAL8 ma1,ma2,a12,a12l;
  REAL8 cosa1=0.;
  REAL8 cosa2=0.;
  REAL8 cosa12=0.;
	
  REAL8 t0=-2.9;
  REAL8 t3=2.6;
  REAL8 s4=-0.123;
  REAL8 s5=0.45;
  REAL8 t2=16.*(0.6865-t3/64.-sqrt(3.)/2.);
	
  /* get a local copy of the intrinstic parameters */
  qq = mass2/mass1;
  eta = mass1*mass2/((mass1+mass2)*(mass1+mass2));
  /* done */
  ma1 = sqrt( s1s1 );
  ma2 = sqrt( s2s2 );
	
  if (ma1>0.) cosa1 = s1L/ma1;
  else cosa1=0.;
  if (ma2>0.) cosa2 = s2L/ma2;
  else cosa2=0.;
  if ((ma1>0.)&&(ma2>0.)) {
    cosa12  = s1s2/ma1/ma2;
  }
  else cosa12=0.;
	
  a12  = ma1*ma1 + ma2*ma2*qq*qq*qq*qq + 2.*ma1*ma2*qq*qq*cosa12 ;
  a12l = ma1*cosa1 + ma2*cosa2*qq*qq ;
  ll = 2.*sqrt(3.)+ t2*eta + t3*eta*eta + s4*a12/(1.+qq*qq)/(1.+qq*qq) + (s5*eta+t0+2.)/(1.+qq*qq)*a12l;
	
  /* Estimate final mass by adding the negative binding energy to the rest mass*/
  *finalMass = 1. + energy;

  /* Estimate final spin */
  *finalSpin = sqrt( a12 + 2.*ll*qq*a12l + ll*ll*qq*qq)/(1.+qq)/(1.+qq);

  /* Check value of finalMass */
  if (*finalMass < 0.) {
    XLALPrintWarning("*** LALSimIMRPSpinInspiralRD ERROR: Estimated final mass <0 : %12.6f\n ",*finalMass);
    XLAL_ERROR( XLAL_ERANGE);
  }
	
  /* Check value of finalSpin */
  if ((*finalSpin > 1.)||(*finalSpin < 0.)) {
    if ((*finalSpin>=1.)&&(*finalSpin<1.01)) {
      XLALPrintWarning("*** LALSimIMRPSpinInspiralRD WARNING: Estimated final Spin slightly >1 : %11.3e\n ",*finalSpin);
      XLALPrintWarning("    (m1=%8.3f  m2=%8.3f  s1sq=%8.3f  s2sq=%8.3f  s1L=%8.3f  s2L=%8.3f  s1s2=%8.3f ) final spin set to 1 and code goes on\n",mass1,mass2,s1s1,s2s2,s1L,s2L,s1s2);
      *finalSpin = .99999;
    }
    else {
      XLALPrintError("*** LALSimIMRPSpinInspiralRD ERROR: Unphysical estimation of final Spin : %11.3e\n ",*finalSpin);
      XLALPrintWarning("    (m1=%8.3f  m2=%8.3f  s1sq=%8.3f  s2sq=%8.3f  s1L=%8.3f  s2L=%8.3f  s1s2=%8.3f )\n",mass1,mass2,s1s1,s2s2,s1L,s2L,s1s2);
      XLALPrintError("***                                    Code aborts\n");
      XLAL_ERROR( XLAL_ERANGE);
    }
  }
	
  return errcode;
} /* End of XLALSimIMRPhenSpinFinalMassSpin*/

static INT4 XLALSimIMRHybridRingdownWave(
    REAL8Vector			*rdwave1,   /**<< Real part of ringdown */
    REAL8Vector			*rdwave2,   /**<< Imaginary part of ringdown */
    const REAL8                 dt,         /**<< Sampling interval */
    const REAL8                 mass1,      /**<< First component mass (in Solar masses) */
    const REAL8                 mass2,      /**<< Second component mass (in Solar masses) */
    REAL8VectorSequence		*inspwave1, /**<< Values and derivatives of real part of inspiral waveform */
    REAL8VectorSequence		*inspwave2, /**<< Values and derivatives of Imaginary part of inspiral waveform */
    COMPLEX16Vector		*modefreqs, /**<< Complex frequencies of ringdown (scaled by total mass) */
    REAL8Vector			*matchrange /**<< Times which determine the comb size for ringdown attachment */
	)
{

  /* XLAL error handling */
  INT4 errcode = XLAL_SUCCESS;

  /* For checking GSL return codes */
  INT4 gslStatus;

  UINT4 i, j, k, nmodes = 8;

  /* Sampling rate from input */
  REAL8 t1, t2, t3, t4, t5, rt;
  gsl_matrix *coef;
  gsl_vector *hderivs;
  gsl_vector *x;
  gsl_permutation *p;
  REAL8Vector *modeamps;
  int s;
  REAL8 tj;
  REAL8 m;

  /* mass in geometric units */
  m  = (mass1 + mass2) * LAL_MTSUN_SI;
  t5 = (matchrange->data[0] - matchrange->data[1]) * m;
  rt = -t5 / 5.;

  //printf("matchrange %16.8e  %16.8e  m %12.4e  t5 %12.4e\n",matchrange->data[0],matchrange->data[1],m,t5);

  t4 = t5 + rt;
  t3 = t4 + rt;
  t2 = t3 + rt;
  t1 = t2 + rt;
  
  if ( inspwave1->length != 3 || inspwave2->length != 3 ||
		modefreqs->length != nmodes )
  {
    XLAL_ERROR( XLAL_EBADLEN );
  }

  /* Solving the linear system for QNMs amplitude coefficients using gsl routine */
  /* Initiate matrices and supporting variables */
  XLAL_CALLGSL( coef = (gsl_matrix *) gsl_matrix_alloc(2 * nmodes, 2 * nmodes) );
  XLAL_CALLGSL( hderivs = (gsl_vector *) gsl_vector_alloc(2 * nmodes) );
  XLAL_CALLGSL( x = (gsl_vector *) gsl_vector_alloc(2 * nmodes) );
  XLAL_CALLGSL( p = (gsl_permutation *) gsl_permutation_alloc(2 * nmodes) );

  /* Check all matrices and variables were allocated */
  if ( !coef || !hderivs || !x || !p )
  {
    if (coef)    gsl_matrix_free(coef);
    if (hderivs) gsl_vector_free(hderivs);
    if (x)       gsl_vector_free(x);
    if (p)       gsl_permutation_free(p);

    XLAL_ERROR( XLAL_ENOMEM );
  }

  /* Define the linear system Ax=y */
  /* Matrix A (2*n by 2*n) has block symmetry. Define half of A here as "coef" */
  /* Define y here as "hderivs" */
  for (i = 0; i < nmodes; ++i)
  {
	gsl_matrix_set(coef, 0, i, 1);
	gsl_matrix_set(coef, 1, i, - cimag(modefreqs->data[i]));
	gsl_matrix_set(coef, 2, i, exp(-cimag(modefreqs->data[i])*t1) * cos(creal(modefreqs->data[i])*t1));
	gsl_matrix_set(coef, 3, i, exp(-cimag(modefreqs->data[i])*t2) * cos(creal(modefreqs->data[i])*t2));
	gsl_matrix_set(coef, 4, i, exp(-cimag(modefreqs->data[i])*t3) * cos(creal(modefreqs->data[i])*t3));
	gsl_matrix_set(coef, 5, i, exp(-cimag(modefreqs->data[i])*t4) * cos(creal(modefreqs->data[i])*t4));
	gsl_matrix_set(coef, 6, i, exp(-cimag(modefreqs->data[i])*t5) * cos(creal(modefreqs->data[i])*t5));
	gsl_matrix_set(coef, 7, i, exp(-cimag(modefreqs->data[i])*t5) * 
		       (-cimag(modefreqs->data[i]) * cos(creal(modefreqs->data[i])*t5)
			-creal(modefreqs->data[i]) * sin(creal(modefreqs->data[i])*t5)));
	gsl_matrix_set(coef, 8, i, 0);
	gsl_matrix_set(coef, 9, i, - creal(modefreqs->data[i]));
	gsl_matrix_set(coef, 10, i, -exp(-cimag(modefreqs->data[i])*t1) * sin(creal(modefreqs->data[i])*t1));
	gsl_matrix_set(coef, 11, i, -exp(-cimag(modefreqs->data[i])*t2) * sin(creal(modefreqs->data[i])*t2));
	gsl_matrix_set(coef, 12, i, -exp(-cimag(modefreqs->data[i])*t3) * sin(creal(modefreqs->data[i])*t3));
	gsl_matrix_set(coef, 13, i, -exp(-cimag(modefreqs->data[i])*t4) * sin(creal(modefreqs->data[i])*t4));
	gsl_matrix_set(coef, 14, i, -exp(-cimag(modefreqs->data[i])*t5) * sin(creal(modefreqs->data[i])*t5));
	gsl_matrix_set(coef, 15, i, exp(-cimag(modefreqs->data[i])*t5) * 
		       ( cimag(modefreqs->data[i]) * sin(creal(modefreqs->data[i])*t5)
			 -creal(modefreqs->data[i]) * cos(creal(modefreqs->data[i])*t5)));
  }
  for (i = 0; i < 2; ++i)
  {
	gsl_vector_set(hderivs, i, inspwave1->data[(i + 1) * inspwave1->vectorLength - 1]);
	gsl_vector_set(hderivs, i + nmodes, inspwave2->data[(i + 1) * inspwave2->vectorLength - 1]);
	gsl_vector_set(hderivs, i + 6, inspwave1->data[i * inspwave1->vectorLength]);
	gsl_vector_set(hderivs, i + 6 + nmodes, inspwave2->data[i * inspwave2->vectorLength]);
  }
  gsl_vector_set(hderivs, 2, inspwave1->data[4]);
  gsl_vector_set(hderivs, 2 + nmodes, inspwave2->data[4]);
  gsl_vector_set(hderivs, 3, inspwave1->data[3]);
  gsl_vector_set(hderivs, 3 + nmodes, inspwave2->data[3]);
  gsl_vector_set(hderivs, 4, inspwave1->data[2]);
  gsl_vector_set(hderivs, 4 + nmodes, inspwave2->data[2]);
  gsl_vector_set(hderivs, 5, inspwave1->data[1]);
  gsl_vector_set(hderivs, 5 + nmodes, inspwave2->data[1]);
  
  /* Complete the definition for the rest half of A */
  for (i = 0; i < nmodes; ++i)
  {
	for (k = 0; k < nmodes; ++k)
	{
	  gsl_matrix_set(coef, i, k + nmodes, - gsl_matrix_get(coef, i + nmodes, k));
	  gsl_matrix_set(coef, i + nmodes, k + nmodes, gsl_matrix_get(coef, i, k));
	}
  }

  #if DEBUG_RD
  printf("\nRingdown matching matrix:\n");
  for (i = 0; i < 16; ++i) {
    for (j = 0; j < 16; ++j) {
      printf("%8.1e ",gsl_matrix_get(coef,i,j));
    }
    printf(" | %8.1e\n",gsl_vector_get(hderivs,i));
  }
  printf("\n");
  #endif

  /* Call gsl LU decomposition to solve the linear system */
  XLAL_CALLGSL( gslStatus = gsl_linalg_LU_decomp(coef, p, &s) );
  if ( gslStatus == GSL_SUCCESS )
  {
    XLAL_CALLGSL( gslStatus = gsl_linalg_LU_solve(coef, p, hderivs, x) );
  }
  if ( gslStatus != GSL_SUCCESS )
  {
    gsl_matrix_free(coef);
    gsl_vector_free(hderivs);
    gsl_vector_free(x);
    gsl_permutation_free(p);
    XLAL_ERROR( XLAL_EFUNC );
  }

  /* Putting solution to an XLAL vector */
  modeamps = XLALCreateREAL8Vector(2 * nmodes);

  if ( !modeamps )
  {
    gsl_matrix_free(coef);
    gsl_vector_free(hderivs);
    gsl_vector_free(x);
    gsl_permutation_free(p);
    XLAL_ERROR( XLAL_ENOMEM );
  }

  for (i = 0; i < nmodes; ++i)
  {
	modeamps->data[i] = gsl_vector_get(x, i);
	modeamps->data[i + nmodes] = gsl_vector_get(x, i + nmodes);
  }

#if DEBUG_RD
  for (i = 0; i < nmodes; ++i)
  {
    printf("%d: om %12.4e  1/tau %12.4e  A %12.4e  B %12.4e \n",i,creal(modefreqs->data[i]),cimag(modefreqs->data[i]),modeamps->data[i],modeamps->data[i + nmodes]);
  }
#endif

  /* Free all gsl linear algebra objects */
  gsl_matrix_free(coef);
  gsl_vector_free(hderivs);
  gsl_vector_free(x);
  gsl_permutation_free(p);

  /* Build ring-down waveforms */
  for (j = 0; j < rdwave1->length; ++j)
  {
	tj = j * dt;
	rdwave1->data[j] = 0;
	rdwave2->data[j] = 0;
	for (i = 0; i < nmodes; ++i)
	{
	  rdwave1->data[j] += exp(- tj * cimag(modefreqs->data[i]))
	    * ( modeamps->data[i] * cos(tj * creal(modefreqs->data[i]))
		+   modeamps->data[i + nmodes] * sin(tj * creal(modefreqs->data[i])) );
	  rdwave2->data[j] += exp(- tj * cimag(modefreqs->data[i]))
	    * (- modeamps->data[i] * sin(tj * creal(modefreqs->data[i]))
			   +   modeamps->data[i + nmodes] * cos(tj * creal(modefreqs->data[i])) );
	}
#if DEBUG_RD
	printf("rd: %d  %12.4e  %12.4e\n",j,rdwave1->data[j],rdwave2->data[j]);
#endif	  
  }

  XLALDestroyREAL8Vector(modeamps);
  return errcode;
}

/**
 * Driver routine for generating PhenSpinRD waveforms
 **/

int XLALSimIMRPhenSpinInspiralRDGenerator(REAL8TimeSeries **hPlus,	         /**< +-polarization waveform [returned] */
					  REAL8TimeSeries **hCross,	         /**< x-polarization waveform [returned] */
					  REAL8 phi_start,                       /**< start phase */
					  REAL8 deltaT,                          /**< sampling interval */
					  REAL8 m1,                              /**< mass of companion 1 in SI units */
					  REAL8 m2,                              /**< mass of companion 2 in SI units */
					  REAL8 f_start,                           /**< start frequency */
					  REAL8 f_ref,                           /**< reference frequency */
					  REAL8 r,                               /**< distance of source */
					  REAL8 iota,                            /**< inclination of source (rad) */
					  REAL8 s1x,                             /**< x-component of dimensionless spin for object 1 */
					  REAL8 s1y,                             /**< y-component of dimensionless spin for object 1 */
					  REAL8 s1z,                             /**< z-component of dimensionless spin for object 1 */
					  REAL8 s2x,                             /**< x-component of dimensionless spin for object 2 */
					  REAL8 s2y,                             /**< y-component of dimensionless spin for object 2 */
					  REAL8 s2z,                             /**< z-component of dimensionless spin for object 2 */
					  int phaseO,                            /**< twice post-Newtonian phase order */
					  int UNUSED ampO,                       /**< twice post-Newtonian amplitude order */
					  LALSimInspiralWaveformFlags *waveFlags,/**< Choice of axis for input spin params */
					  LALSimInspiralTestGRParam *testGRparams/**< Non-GR params */
					  )
{

  int errcode=0;
  int errcodeInt=0;
  uint lengthH=0;     /* Length of hPlus and hCross passed, 0 if NULL*/
  uint intLen;        /* Length of arrays after integration*/
  int idx,jdx,kdx;
  LALSimInspiralSpinTaylorT4Coeffs params;
  REAL8 S1S1=s1x*s1x+s1y*s1y+s1z*s1z;
  REAL8 S2S2=s1x*s1x+s1y*s1y+s1z*s1z;
  REAL8 mass1=m1/LAL_MSUN_SI;
  REAL8 mass2=m2/LAL_MSUN_SI;
  REAL8 Mass=mass1+mass2;

  REAL8 yinit[LAL_NUM_PST4_VARIABLES];
  yinit[0] = phi_start;
  yinit[1] = 0.;
  yinit[2] = 0.;
  yinit[3] = 0.;
  yinit[4] = cos(iota);
  yinit[5] = s1x;
  yinit[6] = s1y;
  yinit[7] = s1z;
  yinit[8] = s2x;
  yinit[9] = s2y;
  yinit[10]= s2z;
  yinit[11]= 0.;

  REAL8TimeSeries *omega, *Phi, *LNhatx, *LNhaty, *LNhatz;
  REAL8TimeSeries *S1x, *S1y, *S1z, *S2x, *S2y, *S2z, *Energy;

  REAL8 tn = XLALSimInspiralTaylorLength(deltaT, m1, m2, f_start, phaseO);
  REAL8 x  = 1.1 * (tn + 1. ) / deltaT;
  int length = ceil(log10(x)/log10(2.));
  lengthH    = pow(2, length);

  if (f_ref<=f_start) {
    errcode=XLALSimIMRPhenSpinInitialize(mass1,mass2,yinit,f_start,-1.,deltaT,phaseO,&params,waveFlags,testGRparams,XLALGetApproximantFromString("PhenSpinTaylorRD"));
    if(errcode) XLAL_ERROR(XLAL_EFUNC);
    if(XLALSimIMRPhenSpinInspiralSetAxis(mass1,mass2,&iota,yinit,XLALSimInspiralGetFrameAxis(waveFlags))) {
      XLAL_ERROR(XLAL_EFUNC);
    }
    errcodeInt=XLALSimInspiralSpinTaylorT4Engine(&omega,&Phi,&LNhatx,&LNhaty,&LNhatz,&S1x,&S1y,&S1z,&S2x,&S2y,&S2z,&Energy,yinit,lengthH,PhenSpinTaylorRD,&params);
    intLen=Phi->data->length;
  }
  else /* do both forward and backward integration*/ {
    REAL8TimeSeries *Phi1, *omega1, *LNhatx1, *LNhaty1, *LNhatz1;
    REAL8TimeSeries *S1x1, *S1y1, *S1z1, *S2x1, *S2y1, *S2z1, *Energy1;
    errcode=XLALSimIMRPhenSpinInitialize(mass1,mass2,yinit,f_ref,f_start,deltaT,phaseO,&params,waveFlags,testGRparams,XLALGetApproximantFromString("PhenSpinTaylorRD"));
    if(errcode) XLAL_ERROR(XLAL_EFUNC);
    if(XLALSimIMRPhenSpinInspiralSetAxis(mass1,mass2,&iota,yinit,XLALSimInspiralGetFrameAxis(waveFlags))) {
      XLAL_ERROR(XLAL_EFUNC);
    }

    REAL8 dyTmp[LAL_NUM_PST4_VARIABLES];
    REAL8 energy;
    XLALSpinInspiralDerivatives(0., yinit,dyTmp,&params);
    energy=dyTmp[11]*params.dt/params.M+yinit[11];
    yinit[11]=energy;

    errcode=XLALSimInspiralSpinTaylorT4Engine(&omega1,&Phi1,&LNhatx1,&LNhaty1,&LNhatz1,&S1x1,&S1y1,&S1z1,&S2x1,&S2y1,&S2z1,&Energy1,yinit,lengthH,PhenSpinTaylorRD,&params);
    /* report on abnormal termination*/
    if ( (errcode != LALSIMINSPIRAL_PHENSPIN_TEST_FREQBOUND) ) {
      XLALPrintError("** LALPSpinInspiralRD WARNING **: integration terminated with code %d.\n",errcode);
      XLALPrintError("   1025: Energy increases\n  1026: Omegadot -ve\n 1028: Omega NAN\n 1029: OMega > OmegaMatch\n 1031: Omega -ve\n");
      XLALPrintError("  Waveform parameters were m1 = %14.6e, m2 = %14.6e, inc = %10.6f,  fref %10.4f Hz\n", mass1, mass2, iota, f_ref);
      XLALPrintError("                           S1 = (%10.6f,%10.6f,%10.6f)\n", s1x, s1y, s1z);
      XLALPrintError("                           S2 = (%10.6f,%10.6f,%10.6f)\n", s2x, s2y, s2z);
    }

    int intLen1=Phi1->data->length;

    yinit[0] = Phi1->data->data[intLen1-1];
    yinit[1] = omega1->data->data[intLen1-1];
    yinit[2] = LNhatx1->data->data[intLen1-1];
    yinit[3] = LNhaty1->data->data[intLen1-1];
    yinit[4] = LNhatz1->data->data[intLen1-1];
    yinit[5] = S1x1->data->data[intLen1-1];
    yinit[6] = S1y1->data->data[intLen1-1];
    yinit[7] = S1z1->data->data[intLen1-1];
    yinit[8] = S2x1->data->data[intLen1-1];
    yinit[9] = S2y1->data->data[intLen1-1];
    yinit[10]= S2z1->data->data[intLen1-1];
    yinit[11]= Energy1->data->data[intLen1-1];

    REAL8TimeSeries *omega2, *Phi2, *LNhatx2, *LNhaty2, *LNhatz2;
    REAL8TimeSeries *S1x2, *S1y2, *S1z2, *S2x2, *S2y2, *S2z2, *Energy2;

    params.fEnd=-1.;
    params.dt*=-1.;
    errcodeInt=XLALSimInspiralSpinTaylorT4Engine(&omega2,&Phi2,&LNhatx2,&LNhaty2,&LNhatz2,&S1x2,&S1y2,&S1z2,&S2x2,&S2y2,&S2z2,&Energy2,yinit,lengthH,PhenSpinTaylorRD,&params);

    REAL8 phiRef=Phi1->data->data[omega1->data->length-1];
    omega =appendTSandFree(omega1,omega2);
    Phi   =appendTSandFree(Phi1,Phi2);
    LNhatx=appendTSandFree(LNhatx1,LNhatx2);
    LNhaty=appendTSandFree(LNhaty1,LNhaty2);
    LNhatz=appendTSandFree(LNhatz1,LNhatz2);
    S1x   =appendTSandFree(S1x1,S1x2);
    S1y   =appendTSandFree(S1y1,S1y2);
    S1z   =appendTSandFree(S1z1,S1z2);
    S2x   =appendTSandFree(S2x1,S2x2);
    S2y   =appendTSandFree(S2y1,S2y2);
    S2z   =appendTSandFree(S2z1,S2z2);
    Energy=appendTSandFree(Energy1,Energy2);
    intLen=Phi->data->length;
    for (idx=0;idx<(int)intLen;idx++) Phi->data->data[idx]-=phiRef;

  } /* End of int forward + integration backward*/

  /* report on abnormal termination*/
  if ( (errcodeInt != LALSIMINSPIRAL_PHENSPIN_TEST_OMEGAMATCH) ) {
      XLALPrintError("** LALPSpinInspiralRD WARNING **: integration terminated with code %d.\n",errcode);
      XLALPrintError("   1025: Energy increases\n  1026: Omegadot -ve\n  1027: Freqbound\n 1028: Omega NAN\n  1031: Omega -ve\n");
      XLALPrintError("  Waveform parameters were m1 = %14.6e, m2 = %14.6e, inc = %10.6f,\n", m1, m2, iota);
      XLALPrintError("                           S1 = (%10.6f,%10.6f,%10.6f)\n", s1x, s1y, s1z);
      XLALPrintError("                           S2 = (%10.6f,%10.6f,%10.6f)\n", s2x, s2y, s2z);
  }

  int count=intLen;
  double tPeak=0.,tm=0.;
  LIGOTimeGPS tStart=LIGOTIMEGPSZERO;
  COMPLEX16Vector* hL2tmp=XLALCreateCOMPLEX16Vector(5);
  COMPLEX16Vector* hL3tmp=XLALCreateCOMPLEX16Vector(7);
  COMPLEX16Vector* hL4tmp=XLALCreateCOMPLEX16Vector(9);
  COMPLEX16TimeSeries* hL2=XLALCreateCOMPLEX16TimeSeries( "hL2", &tStart, 0., deltaT, &lalDimensionlessUnit, 5*lengthH);
  COMPLEX16TimeSeries* hL3=XLALCreateCOMPLEX16TimeSeries( "hL3", &tStart, 0., deltaT, &lalDimensionlessUnit, 7*lengthH);
  COMPLEX16TimeSeries* hL4=XLALCreateCOMPLEX16TimeSeries( "hL4", &tStart, 0., deltaT, &lalDimensionlessUnit, 9*lengthH);
  for (idx=0;idx<(int)hL2->data->length;idx++) hL2->data->data[idx]=0.;
  for (idx=0;idx<(int)hL3->data->length;idx++) hL3->data->data[idx]=0.;
  for (idx=0;idx<(int)hL4->data->length;idx++) hL4->data->data[idx]=0.;

  REAL8TimeSeries *hPtmp=XLALCreateREAL8TimeSeries( "hPtmp", &tStart, 0., deltaT, &lalDimensionlessUnit, lengthH);
  REAL8TimeSeries *hCtmp=XLALCreateREAL8TimeSeries( "hCtmp", &tStart, 0., deltaT, &lalDimensionlessUnit, lengthH);
  COMPLEX16TimeSeries *hLMtmp=XLALCreateCOMPLEX16TimeSeries( "hLMtmp", &tStart, 0., deltaT, &lalDimensionlessUnit, lengthH);
  for (idx=0;idx<(int)hPtmp->data->length;idx++) {
    hPtmp->data->data[idx]=0.;
    hCtmp->data->data[idx]=0.;
    hLMtmp->data->data[idx]=0.;
  }

  LALSimInspiralInclAngle trigAngle;

  REAL8 amp22ini = -2.0 * m1*m2/(m1+m2) * LAL_G_SI/pow(LAL_C_SI,2.) / r * sqrt(16. * LAL_PI / 5.);
  REAL8 amp33ini = -amp22ini * sqrt(5./42.)/4.;
  REAL8 amp44ini = amp22ini * sqrt(5./7.) * 2./9.;
  REAL8 alpha,v,v2,Psi,om;
  REAL8 eta=m1*m2/(m1+m2)/(m1+m2);
  REAL8 dm=(m1-m2)/(m1+m2);
  LALSimInspiralModesChoice modesChoice=XLALSimInspiralGetModesChoice(waveFlags);

  for (idx=0;idx<(int)intLen;idx++) {
    om=omega->data->data[idx];
    v=cbrt(om);
    v2=v*v;
    Psi=Phi->data->data[idx] -2.*om*(1.-eta*v2)*log(om);
    errcode =XLALSimInspiralComputeAlpha(params,LNhatx->data->data[idx],LNhaty->data->data[idx],S1x->data->data[idx],S1y->data->data[idx],S2x->data->data[idx],S2y->data->data[idx],&alpha);
    errcode+=XLALSimInspiralComputeInclAngle(LNhatz->data->data[idx],&trigAngle);
    errcode+=XLALSimSpinInspiralFillL2Modes(hL2tmp,v,eta,dm,Psi,alpha,&trigAngle);
    for (kdx=0;kdx<5;kdx++) {
      hL2->data->data[5*idx+kdx]=hL2tmp->data[kdx]*amp22ini*v2;
    }
    errcode+=XLALSimSpinInspiralFillL3Modes(hL3tmp,v,eta,dm,Psi,alpha,&trigAngle);
    for (kdx=0;kdx<7;kdx++) hL3->data->data[7*idx+kdx]=hL3tmp->data[kdx]*amp33ini*v2;
    errcode+=XLALSimSpinInspiralFillL4Modes(hL4tmp,v,eta,dm,Psi,alpha,&trigAngle);
    for (kdx=0;kdx<9;kdx++) hL4->data->data[9*idx+kdx]=hL4tmp->data[kdx]*amp44ini*v2*v2;
  }

  if (errcodeInt==LALSIMINSPIRAL_PHENSPIN_TEST_OMEGAMATCH) {
    REAL8 LNhS1,LNhS2,S1S2;
    REAL8 omegaMatch;
    REAL8 m1ByMsq=pow(params.m1ByM,2.);    
    REAL8 m2ByMsq=pow(params.m2ByM,2.);
    INT4 iMatch=0;
    INT4 nPts=minIntLen;
    idx=intLen;
    do {
      idx--;
      LNhS1=(LNhatx->data->data[idx]*S1x->data->data[idx]+LNhaty->data->data[idx]*S1y->data->data[idx]+LNhatz->data->data[idx]*S1z->data->data[idx])/m1ByMsq;
      LNhS2=(LNhatx->data->data[idx]*S2x->data->data[idx]+LNhaty->data->data[idx]*S2y->data->data[idx]+LNhatz->data->data[idx]*S2z->data->data[idx])/m2ByMsq;
      S1S2=(S1x->data->data[idx]*S2x->data->data[idx]+S1y->data->data[idx]*S2y->data->data[idx]+S1z->data->data[idx]*S2z->data->data[idx])/m1ByMsq/m2ByMsq;
      omegaMatch=OmMatch(LNhS1,LNhS2,S1S1,S1S2,S2S2);
      if ((omegaMatch>omega->data->data[idx])&&(omega->data->data[idx]<0.1)) {
	if (omega->data->data[idx-1]<omega->data->data[idx]) iMatch=idx;
	// The numerical integrator sometimes stops and stores twice the last
	// omega value, this 'if' instruction avoids keeping two identical 
	// values of omega at the end of the integration.
      }
    } while ((idx>0)&&(iMatch==0));

    INT4 iCpy=0;
    while ( (iMatch+iCpy) < ((int)intLen-1) ) {
      if ((omega->data->data[iMatch+iCpy+1]>omega->data->data[iMatch+iCpy])&&(omega->data->data[iMatch+iCpy+1]<0.1)&&(iCpy<5) ) iCpy++;
      else break;
    }

    //We keep until the point where omega > omegaMatch for better derivative
    //computation, but do the matching at the last point at which
    // omega < omegaMatch

    REAL8Vector *omega_s   = XLALCreateREAL8Vector(nPts);
    REAL8Vector *LNhx_s    = XLALCreateREAL8Vector(nPts);
    REAL8Vector *LNhy_s    = XLALCreateREAL8Vector(nPts);
    REAL8Vector *LNhz_s    = XLALCreateREAL8Vector(nPts);
		
    REAL8Vector *domega    = XLALCreateREAL8Vector(nPts);
    REAL8Vector *dLNhx     = XLALCreateREAL8Vector(nPts);
    REAL8Vector *dLNhy     = XLALCreateREAL8Vector(nPts);
    REAL8Vector *dLNhz     = XLALCreateREAL8Vector(nPts);
    //REAL8Vector *diota     = XLALCreateREAL8Vector(nPts);
    REAL8Vector *dalpha    = XLALCreateREAL8Vector(nPts);
		
    REAL8Vector *ddomega   = XLALCreateREAL8Vector(nPts);
    //REAL8Vector *ddiota    = XLALCreateREAL8Vector(nPts);
    REAL8Vector *ddalpha   = XLALCreateREAL8Vector(nPts);

    int jMatch=nPts-iCpy-1;		
    idx=iMatch-nPts+iCpy+1;
    for (jdx=0;jdx<nPts;jdx++) {
      omega_s->data[jdx]  = omega->data->data[idx];
      LNhx_s->data[jdx]   = LNhatx->data->data[idx];
      LNhy_s->data[jdx]   = LNhaty->data->data[idx];
      LNhz_s->data[jdx]   = LNhatz->data->data[idx];
      idx++;
    }

    errcode  = XLALGenerateWaveDerivative(domega,omega_s,deltaT);
    errcode += XLALGenerateWaveDerivative(dLNhx,LNhx_s,deltaT);
    errcode += XLALGenerateWaveDerivative(dLNhy,LNhy_s,deltaT);
    errcode += XLALGenerateWaveDerivative(dLNhz,LNhz_s,deltaT);
    errcode += XLALGenerateWaveDerivative(ddomega,domega,deltaT);

    for (idx=0;idx<(int)domega->length;idx++)
    if ( (errcode != 0) || (domega->data[jMatch]<0.) || (ddomega->data[jMatch]<0.) ) {
      XLALPrintError("**** LALSimIMRPhenSpinInspiralRD ERROR ****: error generating derivatives");
      XLALPrintError("                     m:           : %12.5f  %12.5f\n",mass1,mass2);
      XLALPrintError("              S1:                 : %12.5f  %12.5f  %12.5f\n",s1x,s1y,s1z);
      XLALPrintError("              S2:                 : %12.5f  %12.5f  %12.5f\n",s2x,s2y,s2z);
      XLALPrintError("     omM %12.5f   om[%d] %12.5f\n",omegaMatch,jMatch,omega);
      XLAL_ERROR(XLAL_EFAILED);
    }
    else {
      REAL8 LNhxy;
      for (idx=0;idx<nPts;idx++) {
	LNhxy = sqrt(LNhx_s->data[idx] * LNhx_s->data[idx] + LNhy_s->data[idx] * LNhy_s->data[idx]);
	if (LNhxy > 0.) {
	  //diota->data[idx]  = -dLNhz->data[idx] / LNhxy;
	  dalpha->data[idx] = (LNhx_s->data[idx] * dLNhy->data[idx] - LNhy_s->data[idx] * dLNhx->data[idx]) / LNhxy;
	} else {
	  //diota->data[idx]  = 0.;
	  dalpha->data[idx] = 0.;
	}
      }
    }
    //errcode += XLALGenerateWaveDerivative(ddiota,diota,deltaT);
    errcode += XLALGenerateWaveDerivative(ddalpha,dalpha,deltaT);

    REAL8 t0;
    tm = t0 = ((REAL8) intLen )*deltaT;
    REAL8 tAs  = t0 + 2. * domega->data[jMatch] / ddomega->data[jMatch];
    REAL8 om1  = domega->data[jMatch] * tAs * (1. - t0 / tAs) * (1. - t0 / tAs);
    REAL8 om0  = omega_s->data[jMatch] - om1 / (1. - t0 / tAs);

    REAL8 dalpha1 = ddalpha->data[jMatch] * tAs * (1. - t0 / tAs) * (1. - t0 / tAs);
    REAL8 dalpha0 = dalpha->data[jMatch] - dalpha1 / (1. - t0 / tAs);

    //printf(" om1 %12.4e  om0 %12.4e\n",om1,om0);
    //printf(" a0 %12.4e  a1 %12.4e\n",dalpha0,dalpha1);

    REAL8 Psi0;
    REAL8 alpha0,energy;

    XLALDestroyREAL8Vector(omega_s);
    XLALDestroyREAL8Vector(LNhx_s);
    XLALDestroyREAL8Vector(LNhy_s);
    XLALDestroyREAL8Vector(LNhz_s);
    XLALDestroyREAL8Vector(domega);
    XLALDestroyREAL8Vector(dLNhx);
    XLALDestroyREAL8Vector(dLNhy);
    XLALDestroyREAL8Vector(dLNhz);
    //XLALDestroyREAL8Vector(diota);
    XLALDestroyREAL8Vector(dalpha);
    XLALDestroyREAL8Vector(ddomega);
    //XLALDestroyREAL8Vector(ddiota);
    XLALDestroyREAL8Vector(ddalpha);

    if ((tAs < t0) || (om1 < 0.)) {
      XLALPrintError("**** LALSimIMRPhenSpinInspiralRD ERROR ****: Could not attach phen part for:\n");
      XLALPrintError(" tAs %12.6e  t0 %12.6e  om1 %12.6e\n",tAs,t0,om1);
      XLALPrintError("   m1 = %14.6e, m2 = %14.6e, inc = %10.6f,\n", mass1, mass2, iota);
      XLALPrintError("   S1 = (%10.6f,%10.6f,%10.6f)\n", s1x, s1y, s1z);
      XLALPrintError("   S2 = (%10.6f,%10.6f,%10.6f)\n", s2x, s2y, s2z);
      XLAL_ERROR(XLAL_EFAILED);
    }
    else /*if Phen part is sane go for this*/ {
      XLALSimInspiralComputeInclAngle(LNhatz->data->data[iMatch],&trigAngle);
      om     = omega->data->data[iMatch];
      Psi    = Phi->data->data[iMatch] - 2. * om * log(om);
      Psi0   = Psi + tAs * (om1/(m1+m2) -dalpha1*trigAngle.ci) * log(1. - t0 / tAs);
      errcode =XLALSimInspiralComputeAlpha(params,LNhatx->data->data[iMatch],LNhaty->data->data[iMatch],S1x->data->data[iMatch],S1y->data->data[iMatch],S2x->data->data[iMatch],S2y->data->data[iMatch],&alpha);
      alpha0 = alpha + tAs * dalpha1 * log(1. - t0 / tAs);
      energy = Energy->data->data[iMatch];
      count  = intLen-1;

      XLALDestroyREAL8TimeSeries(omega);
      XLALDestroyREAL8TimeSeries(Phi);
      XLALDestroyREAL8TimeSeries(LNhatx);
      XLALDestroyREAL8TimeSeries(LNhaty);
      XLALDestroyREAL8TimeSeries(LNhatz);
      XLALDestroyREAL8TimeSeries(S1x);
      XLALDestroyREAL8TimeSeries(S1y);
      XLALDestroyREAL8TimeSeries(S1z);
      XLALDestroyREAL8TimeSeries(S2x);
      XLALDestroyREAL8TimeSeries(S2y);
      XLALDestroyREAL8TimeSeries(S2z);
      XLALDestroyREAL8TimeSeries(Energy);
 
      /* Estimate final mass and spin*/
      REAL8 finalMass,finalSpin;
      errcode=XLALSimIMRPhenSpinFinalMassSpin(&finalMass,&finalSpin,m1,m2,S1S1,S2S2,LNhS1,LNhS2,S1S2,energy);

      /* Get QNM frequencies */
      COMPLEX16Vector *modefreqs=XLALCreateCOMPLEX16Vector(1);
      errcode+=XLALSimIMRPhenSpinGenerateQNMFreq(modefreqs, 2, 2, finalMass, finalSpin, Mass);
      if (errcode) {
        XLALPrintError("**** LALSimIMRPhenSpinInspiralRD ERROR ****: impossible to generate RingDown frequency\n");
        XLALPrintError( "   m  (%11.4e  %11.4e)  f0 %11.4e\n",mass1, mass2, f_start);
        XLALPrintError( "   S1 (%8.4f  %8.4f  %8.4f)\n", s1x,s1y,s1z);
        XLALPrintError( "   S2 (%8.4f  %8.4f  %8.4f)\n", s2x,s2y,s2z);
        XLALDestroyCOMPLEX16Vector(modefreqs);
        XLAL_ERROR(XLAL_EFAILED);
      }

      REAL8 omegaRD = creal(modefreqs->data[0])*Mass*LAL_MTSUN_SI/LAL_PI;
      //printf("  omRD %12.4e  Mass %12.4e\n",omegaRD,Mass);
      REAL8 frOmRD  = fracRD(LNhS1,LNhS2,S1S1,S1S2,S2S2)*omegaRD;

      do {
	count++;
        tm += deltaT;
        om    = om1 / (1. - tm / tAs) + om0;
        Psi   = Psi0 + (- tAs * (om1/Mass-dalpha1*trigAngle.ci) * log(1. - tm / tAs) + (om0/Mass-dalpha0*trigAngle.ci) * (tm - t0) );
        alpha = alpha0 + ( dalpha0 * (tm - t0) - dalpha1 * tAs * log(1. - tm / tAs) );
        v     = cbrt(om);
        v2    = v*v;

	//printf(" %d  om %12.4e  fomRd %12.4e  dotP %12.4e\n",count,om,frOmRD,(om1/Mass-dalpha1*trigAngle.ci)/(1.-tm/tAs)+om0/Mass-dalpha0*trigAngle.ci);

	errcode=XLALSimSpinInspiralFillL2Modes(hL2tmp,v,eta,dm,Psi,alpha,&trigAngle);
	for (kdx=0;kdx<5;kdx++) hL2->data->data[5*count+kdx]=hL2tmp->data[kdx]*amp22ini*v2;
	errcode+=XLALSimSpinInspiralFillL3Modes(hL3tmp,v,eta,dm,Psi,alpha,&trigAngle);
	for (kdx=0;kdx<7;kdx++) hL3->data->data[7*count+kdx]=hL3tmp->data[kdx]*amp33ini*v2;
	errcode+=XLALSimSpinInspiralFillL4Modes(hL4tmp,v,eta,dm,Psi,alpha,&trigAngle);
	for (kdx=0;kdx<9;kdx++) hL4->data->data[9*count+kdx]=hL4tmp->data[kdx]*amp44ini*v2*v2;

      } while ( (om < frOmRD) && (tm < tAs) );

      XLALDestroyCOMPLEX16Vector(hL2tmp);
      XLALDestroyCOMPLEX16Vector(hL3tmp);
      XLALDestroyCOMPLEX16Vector(hL4tmp);

      //printf("Dt %12.4e\n",tm-t0);

      tPeak=tm;

      /*--------------------------------------------------------------
       * Attach the ringdown waveform to the end of inspiral
       -------------------------------------------------------------*/

      static const int nPtsComb=6;
      REAL8Vector *waveR   = XLALCreateREAL8Vector( nPtsComb+2 );
      REAL8Vector *dwaveR  = XLALCreateREAL8Vector( nPtsComb+2 );
      REAL8Vector *ddwaveR = XLALCreateREAL8Vector( nPtsComb+2 );
      REAL8Vector *waveI   = XLALCreateREAL8Vector( nPtsComb+2 );
      REAL8Vector *dwaveI  = XLALCreateREAL8Vector( nPtsComb+2 );
      REAL8Vector *ddwaveI = XLALCreateREAL8Vector( nPtsComb+2 );
      REAL8VectorSequence *inspWaveR = XLALCreateREAL8VectorSequence( 3, nPtsComb );
      REAL8VectorSequence *inspWaveI = XLALCreateREAL8VectorSequence( 3, nPtsComb );

      int nRDWave = (INT4) (RD_EFOLDS / fabs(cimag(modefreqs->data[0])) / deltaT);

      REAL8Vector *matchrange=XLALCreateREAL8Vector(3);
      matchrange->data[0]=(count-nPtsComb+1)*deltaT/(mass1 + mass2) / LAL_MTSUN_SI;
      matchrange->data[1]=count*deltaT/(mass1 + mass2) / LAL_MTSUN_SI;;
      matchrange->data[2]=0.;

     /* Check memory was allocated */
      if ( !waveR || !dwaveR || !ddwaveR || !waveI || !dwaveI || !ddwaveI || !inspWaveR || !inspWaveI ) {
        XLALDestroyCOMPLEX16Vector( modefreqs );
        if (waveR)   XLALDestroyREAL8Vector( waveR );
        if (dwaveR)  XLALDestroyREAL8Vector( dwaveR );
        if (ddwaveR) XLALDestroyREAL8Vector( ddwaveR );
        if (waveI)   XLALDestroyREAL8Vector( waveI );
        if (dwaveI)  XLALDestroyREAL8Vector( dwaveI );
        if (ddwaveI) XLALDestroyREAL8Vector( ddwaveI );
        if (inspWaveR) XLALDestroyREAL8VectorSequence( inspWaveR );
        if (inspWaveI) XLALDestroyREAL8VectorSequence( inspWaveI );
        XLAL_ERROR( XLAL_ENOMEM );
      }

      int l,m;
      int startComb=count-nPtsComb-1;
      count+=nRDWave;

      if ( ( modesChoice &  LAL_SIM_INSPIRAL_MODES_CHOICE_RESTRICTED) ==  LAL_SIM_INSPIRAL_MODES_CHOICE_RESTRICTED ) {
	REAL8Vector *rdwave1l2 = XLALCreateREAL8Vector( nRDWave );
	REAL8Vector *rdwave2l2 = XLALCreateREAL8Vector( nRDWave );
	memset( rdwave1l2->data, 0, rdwave1l2->length * sizeof( REAL8 ) );
	memset( rdwave2l2->data, 0, rdwave2l2->length * sizeof( REAL8 ) );
	l=2;
	for (m=-l;m<=l;m++) {
	  for (idx=0;idx<nPtsComb+2;idx++) {
	    waveR->data[idx]=creal(hL2->data->data[5*(startComb+idx)+(m+l)]);
	    waveI->data[idx]=cimag(hL2->data->data[5*(startComb+idx)+(m+l)]);
	  }
	  errcode =XLALGenerateWaveDerivative(dwaveR,waveR,deltaT);
	  errcode+=XLALGenerateWaveDerivative(ddwaveR,dwaveR,deltaT);
	  errcode+=XLALGenerateWaveDerivative(dwaveI,waveI,deltaT);
	  errcode+=XLALGenerateWaveDerivative(ddwaveI,dwaveI,deltaT);
	  for (idx=0;idx<nPtsComb;idx++) {
	    inspWaveR->data[idx]            =waveR->data[idx+1];
	    inspWaveR->data[idx+  nPtsComb] =dwaveR->data[idx+1];
	    inspWaveR->data[idx+2*nPtsComb] =ddwaveR->data[idx+1];
	    inspWaveI->data[idx]            =waveI->data[idx+1];
	    inspWaveI->data[idx+  nPtsComb] =dwaveI->data[idx+1];
	    inspWaveI->data[idx+2*nPtsComb] =ddwaveI->data[idx+1];
	  }

	  XLALDestroyCOMPLEX16Vector(modefreqs);
	  modefreqs=XLALCreateCOMPLEX16Vector(nModes);
	  errcode+=XLALSimIMRPhenSpinGenerateQNMFreq(modefreqs, l, abs(m), finalMass, dsign(m)*finalSpin, Mass);
#if DEBUG_RD
	  printf("l %d  m %d %d  M %12.4e  S %12.4e\n",l,m,abs(m),finalMass,dsign(m)*finalSpin);
#endif
	  errcode+=XLALSimIMRHybridRingdownWave(rdwave1l2,rdwave2l2,deltaT,mass1,mass2,inspWaveR,inspWaveI,modefreqs,matchrange);
	  for (idx=0;idx<count;idx++) {
	    hLMtmp->data->data[idx]=hL2->data->data[5*idx+(l+m)];
	  }
	  for (idx=count; idx<count+nRDWave;idx++) hLMtmp->data->data[idx]=rdwave1l2->data[idx-count]+I*rdwave2l2->data[idx-count];
	  XLALSimAddMode(hPtmp,hCtmp,hLMtmp,iota,0.,l,m,0);
	}
	XLALDestroyREAL8Vector(rdwave1l2);
	XLALDestroyREAL8Vector(rdwave2l2);
      }
      XLALDestroyCOMPLEX16TimeSeries(hL2);
      if ( ( modesChoice &  LAL_SIM_INSPIRAL_MODES_CHOICE_3L) ==  LAL_SIM_INSPIRAL_MODES_CHOICE_3L ) {
	REAL8Vector *rdwave1l3 = XLALCreateREAL8Vector( nRDWave );
	REAL8Vector *rdwave2l3 = XLALCreateREAL8Vector( nRDWave );
	l=3;
	for (m=-l;m<=l;m++) {
	  for (idx=0;idx<nPtsComb+2;idx++) {
	    waveR->data[idx]=creal(hL3->data->data[7*(startComb+idx)+(m+l)]);
	    waveI->data[idx]=cimag(hL3->data->data[7*(startComb+idx)+(m+l)]);
	  }
	  errcode =XLALGenerateWaveDerivative(waveR,dwaveR,deltaT);
	  errcode+=XLALGenerateWaveDerivative(dwaveR,ddwaveR,deltaT);
	  errcode+=XLALGenerateWaveDerivative(waveI,dwaveI,deltaT);
	  errcode+=XLALGenerateWaveDerivative(dwaveI,ddwaveI,deltaT);
	  for (idx=0;idx<nPtsComb;idx++) {
	    inspWaveR->data[idx]            = waveR->data[idx+1];
	    inspWaveR->data[idx+  nPtsComb] = dwaveR->data[idx+1];
	    inspWaveR->data[idx+2*nPtsComb] = ddwaveR->data[idx+1];
	    inspWaveI->data[idx]            = waveI->data[idx+1];
	    inspWaveI->data[idx+  nPtsComb] = dwaveI->data[idx+1];
	    inspWaveI->data[idx+2*nPtsComb] = ddwaveI->data[idx+1];
	  }
	  errcode+=XLALSimIMRPhenSpinGenerateQNMFreq(modefreqs, l, abs(m), finalMass, dsign(m)*finalSpin, Mass);
#if DEBUG_RD
	  printf("l %d  m %d\n",l,m);
#endif
	  errcode+=XLALSimIMRHybridRingdownWave(rdwave1l3,rdwave2l3,deltaT,mass1,mass2,inspWaveR,inspWaveI,modefreqs,matchrange);
	  for (idx=intLen;idx<count;idx++)  hLMtmp->data->data[idx]=hL3->data->data[7*idx+(l+m)];
	  for (idx=count;idx<nRDWave;idx++) hLMtmp->data->data[idx]=rdwave1l3->data[idx-count]+I*rdwave2l3->data[idx-count];
	  XLALSimAddMode(hPtmp,hCtmp,hLMtmp,iota,0.,l,m,0);
	}
	XLALDestroyREAL8Vector(rdwave1l3);
	XLALDestroyREAL8Vector(rdwave2l3);
      }
      XLALDestroyCOMPLEX16TimeSeries(hL3);
      if ( ( modesChoice &  LAL_SIM_INSPIRAL_MODES_CHOICE_4L) ==  LAL_SIM_INSPIRAL_MODES_CHOICE_4L ) {
	REAL8Vector *rdwave1l4 = XLALCreateREAL8Vector( nRDWave );
	REAL8Vector *rdwave2l4 = XLALCreateREAL8Vector( nRDWave );
	l=4;
	for (m=-l;m<=l;m++) {
	  for (idx=0;idx<nPtsComb+2;idx++) {
	    waveR->data[idx]=creal(hL2->data->data[9*(startComb+idx)+(m+l)]);
	    waveI->data[idx]=cimag(hL2->data->data[9*(startComb+idx)+(m+l)]);
	  }
	  errcode =XLALGenerateWaveDerivative(waveR,dwaveR,deltaT);
	  errcode+=XLALGenerateWaveDerivative(dwaveR,ddwaveR,deltaT);
	  errcode+=XLALGenerateWaveDerivative(waveI,dwaveI,deltaT);
	  errcode+=XLALGenerateWaveDerivative(dwaveI,ddwaveI,deltaT);
	  for (idx=0;idx<nPtsComb;idx++) {
	    inspWaveR->data[idx]            = waveR->data[idx+1];
	    inspWaveR->data[idx+  nPtsComb] = dwaveR->data[idx+1];
	    inspWaveR->data[idx+2*nPtsComb] = ddwaveR->data[idx+1];
	    inspWaveI->data[idx]            = waveI->data[idx+1];
	    inspWaveI->data[idx+  nPtsComb] = dwaveI->data[idx+1];
	    inspWaveI->data[idx+2*nPtsComb] = ddwaveI->data[idx+1];
	  }
	  errcode+= XLALSimIMRPhenSpinGenerateQNMFreq(modefreqs,l,abs(m), finalMass, dsign(m)*finalSpin, Mass);
#if DEBUG_RD
	  printf("l %d  m %d\n",l,m);
#endif
	  errcode+= XLALSimIMRHybridRingdownWave(rdwave1l4,rdwave2l4,deltaT,mass1,mass2,inspWaveR,
	                      inspWaveI,modefreqs,matchrange);
	  for (idx=intLen;idx<count;idx++) hLMtmp->data->data[idx-intLen]=hL4->data->data[9*idx+(l+m)];
	  for (idx=0;idx<nRDWave;idx++)    hLMtmp->data->data[count-intLen+idx]=rdwave1l4->data[idx]+I*rdwave2l4->data[idx];
	  XLALSimAddMode(hPtmp,hCtmp,hLMtmp,iota,0.,l,m,0);
	}
	XLALDestroyREAL8Vector(rdwave1l4);
	XLALDestroyREAL8Vector(rdwave2l4);
      }
      XLALDestroyCOMPLEX16TimeSeries(hL4);
      XLALDestroyCOMPLEX16TimeSeries(hLMtmp);
      XLALDestroyREAL8Vector(matchrange);

      XLALDestroyCOMPLEX16Vector( modefreqs );
      XLALDestroyREAL8Vector( waveR );
      XLALDestroyREAL8Vector( dwaveR );
      XLALDestroyREAL8Vector( ddwaveR );
      XLALDestroyREAL8Vector( waveI );
      XLALDestroyREAL8Vector( dwaveI );
      XLALDestroyREAL8Vector( ddwaveI );
      XLALDestroyREAL8VectorSequence( inspWaveR );
      XLALDestroyREAL8VectorSequence( inspWaveI );
      if (errcode) XLAL_ERROR( XLAL_EFUNC );

    } /* End of: if phen part not sane*/

  } /*End of if intreturn==LALPSIRDPN_TEST_OMEGAMATCH*/
  else {
    if (omega) XLALDestroyREAL8TimeSeries(omega);
    if (Phi) XLALDestroyREAL8TimeSeries(Phi);
    if (LNhatx) XLALDestroyREAL8TimeSeries(LNhatx);
    if (LNhaty) XLALDestroyREAL8TimeSeries(LNhaty);
    if (LNhatz) XLALDestroyREAL8TimeSeries(LNhatz);
    if (S1x) XLALDestroyREAL8TimeSeries(S1x);
    if (S1y) XLALDestroyREAL8TimeSeries(S1y);
    if (S1z) XLALDestroyREAL8TimeSeries(S1z);
    if (S2x) XLALDestroyREAL8TimeSeries(S2x);
    if (S2y) XLALDestroyREAL8TimeSeries(S2y);
    if (S2z) XLALDestroyREAL8TimeSeries(S2z);
    if (Energy) XLALDestroyREAL8TimeSeries(Energy);
  }

  if ((*hPlus) && (*hCross)) {
    if ((*hPlus)->data->length!=(*hCross)->data->length) {
      XLALPrintError("*** LALSimIMRPSpinInspiralRD ERROR: h+ and hx differ in length: %d vs. %d\n",(*hPlus)->data->length,(*hCross)->data->length);
      XLAL_ERROR(XLAL_EFAILED);
    }
    else {
      if ((int)(*hPlus)->data->length<count) {
	XLALPrintError("*** LALSimIMRPSpinInspiralRD ERROR: h+ and hx too short: %d vs. %d\n",(*hPlus)->data->length,count);
	XLAL_ERROR(XLAL_EFAILED);
      }
      else {
	XLALGPSAdd(&((*hPlus)->epoch),-tPeak);
	XLALGPSAdd(&((*hCross)->epoch),-tPeak);
      }
    }
  }
  else {
    XLALGPSAdd(&tStart,-tPeak);
    *hPlus  = XLALCreateREAL8TimeSeries("H+", &tStart, 0.0, deltaT, &lalDimensionlessUnit, count);
    *hCross = XLALCreateREAL8TimeSeries("Hx", &tStart, 0.0, deltaT, &lalDimensionlessUnit, count);
    if(*hPlus == NULL || *hCross == NULL)
      XLAL_ERROR(XLAL_ENOMEM);
  }

  int minLen=hPtmp->data->length < (*hPlus)->data->length ? hPtmp->data->length : (*hPlus)->data->length;
  for (idx=0;idx<minLen;idx++) {
    (*hPlus)->data->data[idx] =hPtmp->data->data[idx];
    (*hCross)->data->data[idx]=hCtmp->data->data[idx];
  }
  for (idx=minLen;idx<(int)(*hPlus)->data->length;idx++) {
    (*hPlus)->data->data[idx] =0.;
    (*hCross)->data->data[idx]=0.;
  }

  XLALDestroyREAL8TimeSeries(hPtmp);
  XLALDestroyREAL8TimeSeries(hCtmp);

  return count;
} /* End of XLALSimIMRPhenSpinInspiralRDGenerator*/
