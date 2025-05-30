.. _raster.mff2:

================================================================================
MFF2 -- Vexcel MFF2 Image
================================================================================

.. shortname:: MFF2

.. built_in_by_default::

GDAL supports MFF2 Image raster file format for read.
The MFF2 (Multi-File Format 2) format was designed to fit into
Vexcel Hierarchical Key-Value (HKV) databases, which can store binary
data as well as ASCII parameters. This format is primarily used
internally to the Vexcel InSAR processing system.

To select an MFF2 dataset, select the directory containing the
``attrib``, and ``image_data`` files for the dataset.

Currently only latitude/longitude and UTM projection are supported
(georef.projection.name = ll or georef.projection.name = utm), with the
affine transform computed from the lat/long control points. In any
event, if GCPs are available in a georef file, they are returned with
the dataset.

For read, all data types (real, integer and complex in bit depths of 8, 16, 32) should
be supported.

NOTE: Implemented as :source_file:`frmts/raw/hkvdataset.cpp`.

Driver capabilities
-------------------

.. supports_georeferencing::

.. supports_virtualio::

Format Details
--------------

MFF2 Top-level Structure
~~~~~~~~~~~~~~~~~~~~~~~~

An MFF2 "file" is actually a set of files stored in a directory
containing an ASCII header file entitled "attrib", and binary image data
entitled "image_data". Optionally, there may be an ASCII "georef" file
containing georeferencing and projection information, and an
"image_data_ovr" (for "image_data" binary image data) file containing
tiled overviews of the image in TIFF format. The ASCII files are
arranged in key=value pairs. The allowable pairs for each file are
described below.

The "attrib" File
~~~~~~~~~~~~~~~~~

As a minimum, the "attrib" file must specify the image extents, pixel
size in bytes, pixel encoding and datatype, and pixel byte order. For
example,

::

   extent.cols    = 800
   extent.rows    = 1040
   pixel.size     = 32
   pixel.encoding = { unsigned twos_complement *ieee_754 }
   pixel.field    = { *real complex }
   pixel.order    = { lsbf *msbf }
   version        = 1.1

specifies an image that is 1040 lines by 800 pixels in extent. The
pixels are 32 bits of real data in "most significant byte first" (msbf)
order, encoded according to the ieee_754 specification. In MFF2, when a
value must belong to a certain subset (eg. pixel.order must be either
lsbf or msbf), all options are displayed between curly brackets, and the
one appropriate for the current file is indicated with a "*".

The file may also contain the following lines indicating the number of
channels of data, and how they are interleaved within the binary data
file.

::

   channel.enumeration = 1
   channel.interleave = { *pixel tile sequential }

The "image_data" File
~~~~~~~~~~~~~~~~~~~~~

The "image_data" file consists of raw binary data, with extents, pixel
encoding, and number of channels as indicated in the "attrib" file.

The "georef" File
~~~~~~~~~~~~~~~~~

The "georef" file is used to describe the geocoding and projection
information for the binary data. For example,

::

   top_left.latitude            = 32.93333333333334
   top_left.longitude           = 130.0
   top_right.latitude           = 32.93333333333334
   top_right.longitude          = 130.5
   bottom_left.latitude         = 32.50000000000001
   bottom_left.longitude        = 130.0
   bottom_right.latitude        = 32.50000000000001
   bottom_right.longitude       = 130.5
   centre.latitude              = 32.71666666666668
   centre.longitude             = 130.25
   projection.origin_longitude  = 0
   projection.name              = ll
   spheroid.name                = wgs-84

describes an orthogonal latitude/longitude (ll) projected image, with
latitudes and longitudes based on the wgs-84 ellipsoid.

Since MFF2 version 1.1, top_left refers to the top left corner of the
top left pixel. top_right refers to the top right corner of the top
right pixel. bottom_left refers to the bottom left corner of the bottom
left pixel. bottom_right refers to the bottom right corner of the bottom
right pixel. centre refers to the centre of the four corners defined
above (center of the image).

Mathematically, for an Npix by Nline image, the corners and centre in
(pixel,line) coordinates for MFF2 version 1.1 are:

::

   top_left: (0,0)
   top_right: (Npix,0)
   bottom_left: (0,Nline)
   bottom_right: (Npix,Nline)
   centre: (Npix/2.0,Nline/2.0)

These calculations are done using floating point arithmetic (i.e. centre
coordinates may take on non-integer values).

Note that the corners are always expressed in latitudes/longitudes, even
for projected images.

Supported projections
~~~~~~~~~~~~~~~~~~~~~

ll- Orthogonal latitude/longitude projected image, with latitude
parallel to the rows, longitude parallel to the columns. Parameters:
spheroid name, projection.origin_longitude (longitude at the origin of
the projection coordinates). If not set, this should default to the
central longitude of the output image based on its projection
boundaries.

utm- Universal Transverse Mercator projected image. Parameters: spheroid
name, projection.origin_longitude (central meridian for the utm
projection). The central meridian must be the meridian at the centre of
a UTM zone, i.e. 3 degrees, 9 degrees, 12 degrees, etc. If this is not
specified or set a valid UTM central meridian, the reader should reset
the value to the nearest valid central meridian based on the central
longitude of the output image. The latitude at the origin of the UTM
projection is always 0 degrees.

Recognized ellipsoids
~~~~~~~~~~~~~~~~~~~~~

MFF2 format associates the following names with ellipsoid equatorial
radius and inverse flattening parameters:

::

   airy-18304:            6377563.396      299.3249646
   modified-airy4:        6377340.189      299.3249646
   australian-national4:  6378160          298.25
   bessel-1841-namibia4:  6377483.865      299.1528128
   bessel-18414:          6377397.155      299.1528128
   clarke-18584:          6378294.0        294.297
   clarke-18664:          6378206.4        294.9786982
   clarke-18804:          6378249.145      293.465
   everest-india-18304:   6377276.345      300.8017
   everest-sabah-sarawak4:6377298.556      300.8017
   everest-india-19564:   6377301.243      300.8017
   everest-malaysia-19694:6377295.664      300.8017
   everest-malay-sing4:   6377304.063      300.8017
   everest-pakistan4:     6377309.613      300.8017
   modified-fisher-19604: 6378155          298.3
   helmert-19064:         6378200          298.3
   hough-19604:           6378270          297
   hughes4:               6378273.0        298.279
   indonesian-1974:       6378160          298.247
   international-1924:    6378388          297
   iugc-67:               6378160.0        298.254
   iugc-75:               6378140.0        298.25298
   krassovsky-1940:       6378245          298.3
   kaula:                 6378165.0        292.308
   grs-80:                6378137          298.257222101
   south-american-1969:   6378160          298.25
   wgs-72:                6378135          298.26
   wgs-84:                6378137          298.257223563
   ev-wgs-84:             6378137          298.252841
   ev-bessel:             6377397          299.1976073

Explanation of fields
~~~~~~~~~~~~~~~~~~~~~

::

   channel.enumeration:  (optional- only needed for multiband)
   Number of channels of data (eg. 3 for rgb)

   channel.interleave = { *pixel tile sequential } :  (optional- only
   needed for multiband)

   For multiband data, indicates how the channels are interleaved.  *pixel
   indicates that data is stored red value, green value, blue value, red
   value, green value, blue value etc. as opposed to (line of red values)
   (line of green values) (line of blue values) or (entire red channel)
   (entire green channel) (entire blue channel)

   extent.cols:
   Number of columns of data.

   extent.rows:
   Number of rows of data.

   pixel.encoding = { *unsigned twos-complement ieee-754 }:
   Combines with pixel.size and pixel.field to give the data type:
   (encoding, field, size)- type
   (unsigned, real, 8)- unsigned byte data
   (unsigned, real, 16)- unsigned int 16 data
   (unsigned, real, 32)- unsigned int 32 data
   (twos-complement, real, 16)- signed int 16 data
   (twos-complement, real, 32)- signed int 32 data
   (twos-complement, complex, 64)- complex signed int 32 data
   (ieee-754, real, 32)- real 32 bit floating point data
   (ieee-754, real, 64)- real 64 bit floating point data
   (ieee-754, complex, 64)- complex 32 bit floating point data
   (ieee-754, complex, 128)- complex 64 bit floating point data

   pixel.size:
   Size of one pixel of one channel (bits).

   pixel.field = { *real complex }:
   Whether the data is real or complex.

   pixel.order = { *lsbf msbf }:
   Byte ordering of the data (least or most significant byte first).

   version: (only in newer versions- if not present, older version is
   assumed) Version of mff2.
