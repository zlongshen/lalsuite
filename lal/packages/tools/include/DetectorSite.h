/********************************* <lalVerbatim file="DetectorSiteHV">
Author: Whelan, J. T.
$Id$
********************************** </lalVerbatim> */

/********************************* <lalLaTeX>

\section{Header \texttt{DetectorSite.h}}
\label{s:DetectorSite.h}

This header defines structures to hold the basic data describing
a gravitational wave detector.  

\subsection*{Synopsis}
\begin{verbatim}
#include <lal/DetectorSite.h>
\end{verbatim}

According to the common frame format specification
\cite{LIGOVIRGO:2000} the geometry of an interferometric detector will
be stored in a \texttt{FrDetector} structure, specifying the location
of the detector vertex and the orientation of its arms in geodetic
co\"{o}rdinates suited to geographical surveying.  Resonant bars and
other sorts of detectors, if they write their data to frames, are
expected to fill this structure with their location and
orientation in some way suited to the detector type.

For most data analysis tasks, however, any gravitational wave detector
can be described by its location in an Earth-fixed rotating reference
frame, as well as a \textit{response tensor} $d^{ab}$, constant in the
same frame, which defines the ``strain'' $h$ measured by the detector in
terms of the metric perturbation $h_{ab}$ as
\begin{equation}
h = h_{ab} \, d^{ab}
\ .
\end{equation}

This header defines a \texttt{LALFrDetector} structure which contains
essentially the same information as the \texttt{FrDetector} structure,
as well as a \texttt{LALDetector} structure which contains the
Cartesian co\"{o}rdinates of the detector along with the components of
the response tensor $d^{ab}$ in the same co\"{o}rdinate system.

\subsubsection*{The Geodetic Co\"{o}rdinate System}

Geodetic co\"{o}rdinates are spheroidal co\"{o}rdinates
based on the WGS-84 Earth Model, which is an
oblate spheroid with equatorial radius $a=6.378137\times
10^6\,\textrm{m}$ and polar radius $b=6.356752314\times
10^6\,\textrm{m}$.  Any point in space can be located according to its
longitude, latitude, and elevation.  The \textit{longitude} $\lambda$
is the angle between the half-plane bounded by the symmetry axis of
the reference ellipsoid containing the point in question and the
half-plane plane containing the Prime Meridian; it is measured in
degrees, increases to the East, and ranges from
$-180^\circ$ to $180^\circ$.  The \textit{latitude} $\beta$ is the
angle between the ray which is normal to the ellipsoid and passes
through the point in question and the equatorial plane; it is measured
in degrees, increases to the North, and ranges
from $-90^\circ$ to $90^\circ$.  The \textit{elevation} $h$ is the
signed distance along this ray from the reference ellipsoid to the
point in question.  This co\"{o}rdinate system is described in more
detail in \cite{Althouse:1999}.

\subsubsection*{Altitude and Azimuth Angles}

The \texttt{LALFrDetector} structure stores the directions along the
two arms of an interferometer in an altitude/azimuth representation
with respect to the local tangent plane to the reference ellipsoid,
known as the local horizontal.  The altitude ${\mathcal{A}}$ is the angle the
direction vector makes with the horizontal, ${\mathcal{A}} > 0$ meaning above
horizontal, ${\mathcal{A}} < 0$ below.  The azimuth angle $\zeta$ is found by
projecting the direction onto the local horizontal plane, then
measuring the angle counter-clockwise from East (the direction in
which longitude increases fastest) to this projected direction.

\subsubsection*{The Cartesian Co\"{o}rdinate System}

The position vector and response tensor contained in the
\texttt{LALDetector} structure are defined in
 a simple orthonormal co\"{o}rdinate system with its origin at
the center of the earth, an $x^1$ axis which pierces the Earth's
surface at the intersection of the equator and the prime meridian, an
$x^2$ axis which pierces the earth's surface at 90 degrees East
longitude on the equator, and an $x^3$ axis which pierces the Earth's
surface at the North Pole.  The co\"{o}rdinates $x^1$, $x^2$, $x^3$
correspond to the Earth-fixed co\"{o}rdinates $X_E$, $Y_E$, $Z_E$
defined in \cite{Althouse:1999}, respectively.

The relationship between geodetic and Cartesian co\"{o}rdinates is
given by
\begin{eqnarray}
\label{e:cart1}
x^1&=&\left(
          \frac{a^2}{\sqrt{a^2\cos^2\beta+b^2\sin^2\beta}}
          + h
       \right) \cos\beta\cos\lambda             \\
\label{e:cart2}
x^2&=&\left(
          \frac{a^2}{\sqrt{a^2\cos^2\beta+b^2\sin^2\beta}}
          + h
       \right) \cos\beta\sin\lambda             \\
\label{e:cart3}
x^3&=&\left(
          \frac{b^2}{\sqrt{a^2\cos^2\beta+b^2\sin^2\beta}}
          + h
       \right) \sin\beta
\end{eqnarray}

\subsection*{Error conditions}
\input{DetectorSiteHE}

********************************** </lalLaTeX> */

/**************************************** <lalLaTeX file="DetectorSiteHB">

\bibitem{LIGOVIRGO:2000}
LIGO Data Group and VIRGO Data Acquisition Group, ``Specification of a
Common Data Frame Format for Interferometric Gravitational Wave
Detectors (IGWD)'', LIGO Technical Note
\href{http://www.ligo.caltech.edu/docs/T/T970130-D.pdf}{LIGO-T970130}

\bibitem{Althouse:1999}
William Althouse, Larry Jones, and Albert Lazzarini, ``Determination
of Global and Local Coordinate Axes for the LIGO Sites'', LIGO
Technical Note
\href{http://www.ligo.caltech.edu/docs/T/T980044-10.pdf}{LIGO-T980044}

% \bibitem{Lazzarini:1995}
% Albert Lazzarini, ``Derivation
% of Global and Local Coordinate Axes for the LIGO Sites'', LIGO
% Technical Note LIGO-T950004

******************************************************* </lalLaTeX> */ 

#ifndef _DETECTORSITE_H
#define _DETECTORSITE_H

#include <lal/LALStdlib.h>

#ifdef  __cplusplus
extern "C" {
#endif

NRCSID (DETECTORSITEH, "$Id$");


/******************************** <lalErrTable file="DetectorSiteHE"> */

#define DETECTORSITEH_ENULLP        1
#define DETECTORSITEH_ETYPE         2

#define DETECTORSITEH_MSGENULLP     "Null pointer"
#define DETECTORSITEH_MSGETYPE      "Unsupported detector type"

/************************************ </lalErrTable> */



/********************************* <lalLaTeX> 

\subsubsection*{The \texttt{LALDetectorType} enumeration}
\index{\texttt{LALDetectorType}}
\index{\texttt{LALABSENTDETECTOR}}
\index{\texttt{LALIFODIFFDETECTOR}}
\index{\texttt{LALIFOXARMDETECTOR}}
\index{\texttt{LALIFOYARMDETECTOR}}
\index{\texttt{LALIFOCOMMDETECTOR}}
\index{\texttt{LALIFOCYLBARDETECTOR}}

Since data from bars as well as interferometers can be written to
   frames, we need an additional piece of information to interpret the
   site geometry data specified in the \texttt{LALFrDetector}
   structure; for instance, is the x arm really the x arm or is it the
   long axis of a bar?  The \texttt{LALDetectorType} enumeration
   provides a way to keep track of that.

The possible values are:
\begin{description}
  \item[\texttt{LALABSENTDETECTOR}] No \texttt{FrDetector} associated with the structure
  \item[\texttt{LALIFODIFFDETECTOR}] Interferometer in differential mode
  \item[\texttt{LALIFOXARMDETECTOR}] Interferometer in one-armed mode (X arm)
  \item[\texttt{LALIFOYARMDETECTOR}] Interferometer in one-armed mode (Y arm)
  \item[\texttt{LALIFOCOMMDETECTOR}] Interferometer in common mode
  \item[\texttt{LALIFOCYLBARDETECTOR}]  Cylindrical bar
%  \item[\texttt{SPHEREMONO}] Resonant sphere in $\ell=0$ mode
%  \item[\texttt{SPHEREQUAD0}] Resonant sphere in $\ell=2$, $m=0$ mode
%  \item[\texttt{SPHEREQUAD1S}] Resonant sphere in $\ell=2$, 
%     $m=\pm 1$ (sine) mode
%  \item[\texttt{SPHEREQUAD1C}] Resonant sphere in $\ell=2$, 
%     $m=\pm 1$ (cosine) mode
%  \item[\texttt{SPHEREQUAD2S}] Resonant sphere in $\ell=2$, 
%     $m=\pm 2$ (sine) mode
%  \item[\texttt{SPHEREQUAD2C}] Resonant sphere in $\ell=2$, 
%     $m=\pm 2$ (cosine) mode
\end{description}

********************************** </lalLaTeX> */

typedef enum {
  LALABSENTDETECTOR,       /* No FrDetector associated with the structure */
  LALIFODIFFDETECTOR,      /* IFO in differential mode */
  LALIFOXARMDETECTOR,      /* IFO in one-armed mode (X arm) */
  LALIFOYARMDETECTOR,      /* IFO in one-armed mode (Y arm) */
  LALIFOCOMMDETECTOR,      /* IFO in common mode */
  LALIFOCYLBARDETECTOR,       /* Cylindrical Bar */
  /* SPHEREMONO,    Resonant Sphere in l=0 mode */
  /* SPHEREQUAD0,   Resonant sphere in l=2, m=0 mode */
  /* SPHEREQUAD1S,  Resonant sphere in l=2, m=+/-1 (sine) mode */
  /* SPHEREQUAD1C,  Resonant sphere in l=2, m=+/-1 (cosine) mode */
  /* SPHEREQUAD2S,  Resonant sphere in l=2, m=+/-2 (sine) mode */
  /* SPHEREQUAD2C,  Resonant sphere in l=2, m=+/-2 (cosine) mode */
  /* etc */
} LALDetectorType;

/********************************* <lalLaTeX> 

\subsubsection*{Cached Detectors}

\index{\texttt{lalCachedDetectors[]}}
In practice, we will often be working with fixed unchanging site
   geometry, e.g., for the LIGO interferometers; to avoid constantly
   reconstructing the corresponding \texttt{LALDetector}s, we should
   define some constant \texttt{LALDetector}s describing them.  Those
   are stored in a constant array of \texttt{LALDetector} structures
   known as \texttt{lalCachedDetectors}, which is declared
   \texttt{extern} in this header and defined in
   \texttt{CreateDetector.c} (see Sec.~\ref{ss:CreateDetector.c}).

The \texttt{LALCreateDetector()} routine will first look through the
   \texttt{lalCachedDetectors} array for a \texttt{LALDetector}
   structure with matching \texttt{type} and \texttt{frDetector.name}
   fields; if it finds one, it returns a copy of that; if not, it
   creates one.

   The header \texttt{DetectorSite.h} also defines an enumeration of
   the indices of the known detectors:
   \index{\texttt{LALDetectorIndexLHODIFF}}
   \index{\texttt{LALDetectorIndexLLODIFF}}
   \index{\texttt{LALNumCachedDetectors}}
********************************** </lalLaTeX> */

/********************************* <lalVerbatim> */
enum
{
  LALDetectorIndexLHODIFF,
  LALDetectorIndexLLODIFF,
  LALNumCachedDetectors
};
/********************************** </lalVerbatim> */


/********************************* <lalLaTeX> 
For example, the \texttt{LALDetector} representing LIGO Hanford in
differential mode is
\texttt{lalCachedDetectors[LALDetectorIndexLHODIFF]}.

\subsection*{Structures}

/********************************** </lalLaTeX> */

/********************************* <lalLaTeX> 

\subsubsection*{Structure \texttt{LALFrDetector}}
\index{\texttt{LALFrDetector}}

The \texttt{LALFrDetector} structure holds site geometry information
in the same format as the \texttt{FrDetector} structure defined in the
frames spec. \cite{LIGOVIRGO:2000}  The fields are:
\begin{description}
  \item[\texttt{CHAR name[LALNameLength]}] A unique identifying string.
  \item[\texttt{REAL8 vertexLongitudeDegrees}] The geodetic longitude 
$\lambda$ of the vertex, in degrees.
  \item[\texttt{REAL8 vertexLatitudeDegrees}] The geodetic latitude
$\beta$ of the vertex, in degrees.
  \item[\texttt{REAL4 vertexElevation}] The height of the vertex above
  the reference ellipsoid, in meters.
  \item[\texttt{REAL4 xArmAltitudeRadians}]  The angle ${\mathcal{A}}_X$ up from the
  local tangent plane of the reference ellipsoid to the X arm, in radians.
  \item[\texttt{REAL4 xArmAzimuthRadians}] The angle $\zeta_X$ counterclockwise
  from East to the projection of the X arm into the local tangent plane of
  the reference ellipsoid, in radians.
  \item[\texttt{REAL4 yArmAltitudeRadians}]  The angle ${\mathcal{A}}_Y$ up from the
  local tangent plane of the reference ellipsoid to the Y arm, in radians.
  \item[\texttt{REAL4 yArmAzimuthRadians}] The angle $\zeta_Y$ counterclockwise
  from East to the projection of the Y arm into the local tangent plane of
  the reference ellipsoid, in radians.
\end{description}

********************************** </lalLaTeX> */


typedef struct tagLALFrDetector
{
  CHAR             name[LALNameLength];
  REAL8            vertexLongitudeDegrees;
  REAL8            vertexLatitudeDegrees;
  REAL4            vertexElevation;
  REAL4            xArmAltitudeRadians;
  REAL4            xArmAzimuthRadians;
  REAL4            yArmAltitudeRadians;
  REAL4            yArmAzimuthRadians;
} LALFrDetector; 

/********************************* <lalLaTeX> 

\subsubsection*{Structure \texttt{LALDetector}}
\index{\texttt{LALDetector}}

The \texttt{LALDetector} structure is intended to be the way that detector
   geometry information is passed to LAL routines. 
This structure describes a detector geometry in a way independent of
the type of detector.  The fields are:
\begin{description}
  \item[\texttt{REAL8 location[3]}]  The three components, in an 
  Earth-fixed Cartesian co\"{o}rdinate system, of the
  position vector from the center of the Earth to the detector,
  in meters.
  \item[\texttt{REAL4 response[3][3]}] The Earth-fixed Cartesian components
 of the detector's response tensor   $d^{ab}$.
  \item[\texttt{LALDetectorType type}] The type of detector (e.g., IFO in
  differential mode, cylindrical bar, etc.)
  \item[\texttt{LALFrDetector frDetector}] The original
  \texttt{LALFrDetector} structure from which this was created.
\end{description}

********************************** </lalLaTeX> */

typedef struct tagLALDetector
{
  REAL8            location[3];
  REAL4            response[3][3];
  LALDetectorType  type;
  LALFrDetector    frDetector;
} LALDetector;

void LALCreateDetector( LALStatus       *status,
			LALDetector     *output,
			LALFrDetector   *input,
			LALDetectorType  type );

extern const LALDetector lalCachedDetectors[LALNumCachedDetectors];

/********************************* <lalLaTeX> 

\vfill{\footnotesize\input{DetectorSiteHV}}
\newpage\input{CreateDetectorC}
\newpage\input{DetectorSiteTestC}

********************************** </lalLaTeX> */

#ifdef  __cplusplus
}
#endif

#endif /* _UNITS_H */
