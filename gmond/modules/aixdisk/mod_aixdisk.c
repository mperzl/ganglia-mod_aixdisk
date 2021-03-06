/******************************************************************************
 *
 *  This module implements IBM AIX disk statistics with libperfstat.
 *
 *  The code has been tested with AIX 5.1, AIX 5.2, AIX 5.3, AIX 6.1
 *  and AIX 7.1 on different systems.
 *
 *  Written by Michael Perzl (michael@perzl.org)
 *
 *  Version 1.0, Nov 20, 2011
 *
 *  Version 1.0:  Nov 20, 2011
 *                - initial version
 *
 ******************************************************************************/

/*
 * The ganglia metric "C" interface, required for building DSO modules.
 */

#include <gm_metric.h>


#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <utmp.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <syslog.h>

#include <apr_tables.h>
#include <apr_strings.h>

#include <libperfstat.h>
#include <sys/var.h>
#include <sys/systemcfg.h>

#include "libmetrics.h"


/* See /usr/include/sys/iplcb.h to explain the below */
#define XINTFRAC ((double)(_system_configuration.Xint)/(double)(_system_configuration.Xfrac))

/* hardware ticks per millisecond */
#define HWTICS2MSECS(x) ((double) x * XINTFRAC)/1000000.0

#ifndef FIRST_DISK
#define FIRST_DISK ""
#endif


/* define for debugging output */
#undef DEBUG

#define MIN_THRESHOLD 5.0

#define MAX_BUF_SIZE 1024


static time_t boottime;


static int allowDiskPerfCollection;


struct aixdisk_t {
   int enabled;
   double last_read;
   double threshold;
   char devName[MAX_G_STRING_SIZE];
};

typedef struct aixdisk_t aixdisk_t;


struct aixdisk_data_t {
   double last_value;
   double curr_value;
   u_longlong_t last_total_value;
};

typedef struct aixdisk_data_t aixdisk_data_t;


static unsigned int aixdisk_count = 0;

static aixdisk_t *aixdisks = NULL;

static aixdisk_data_t *aixdisk_size = NULL;
static aixdisk_data_t *aixdisk_free = NULL;
static aixdisk_data_t *aixdisk_bsize = NULL;
static aixdisk_data_t *aixdisk_xrate = NULL;
static aixdisk_data_t *aixdisk_xfers = NULL;
static aixdisk_data_t *aixdisk_wbytes = NULL;
static aixdisk_data_t *aixdisk_rbytes = NULL;
static aixdisk_data_t *aixdisk_qdepth = NULL;
static aixdisk_data_t *aixdisk_time = NULL;
#ifdef _AIX53
static aixdisk_data_t *aixdisk_q_full = NULL;
static aixdisk_data_t *aixdisk_rserv = NULL;
static aixdisk_data_t *aixdisk_rtimeout = NULL;
static aixdisk_data_t *aixdisk_rfailed = NULL;
static aixdisk_data_t *aixdisk_min_rserv = NULL;
static aixdisk_data_t *aixdisk_max_rserv = NULL;
static aixdisk_data_t *aixdisk_wserv = NULL;
static aixdisk_data_t *aixdisk_wtimeout = NULL;
static aixdisk_data_t *aixdisk_wfailed = NULL;
static aixdisk_data_t *aixdisk_min_wserv = NULL;
static aixdisk_data_t *aixdisk_max_wserv = NULL;
static aixdisk_data_t *aixdisk_wq_depth = NULL;
static aixdisk_data_t *aixdisk_wq_sampled = NULL;
static aixdisk_data_t *aixdisk_wq_time = NULL;
static aixdisk_data_t *aixdisk_wq_min_time = NULL;
static aixdisk_data_t *aixdisk_wq_max_time = NULL;
#endif

static apr_pool_t *pool;

static apr_array_header_t *metric_info = NULL;



static time_t
boottime_func_CALLED_ONCE( void )
{
   time_t boottime;
   struct utmp buf;
   FILE *utmp;


   utmp = fopen( UTMP_FILE, "r" );

   if (utmp == NULL)
   {
      /* Can't open utmp, use current time as boottime */
      boottime = time( NULL );
   }
   else
   {
      while (fread( (char *) &buf, sizeof( buf ), 1, utmp ) == 1)
      {
         if (buf.ut_type == BOOT_TIME)
         {
            boottime = buf.ut_time;
            break;
        }
      }

      fclose( utmp );
   }

   return( boottime );
}



static int
detect_aixdisk_devices( void )
{
   int count,
       i;
   perfstat_disk_t *p;
   perfstat_id_t name;


/* find out the number of available AIX disks */

   count = perfstat_disk( NULL, NULL, sizeof( perfstat_disk_t ), 0 );
   if (count == -1)
      count = 0;

#ifdef DEBUG
fprintf( stderr, "Found AIX disks = %d\n", count );  fflush( stderr );
#endif

   if (count > 0)
   {
/* allocate enough memory for all the structures */
      p = malloc( sizeof( perfstat_disk_t ) * count );

/* ask to get all the structures available in one call */
/* return code is number of structures returned */
      strcpy( name.name, FIRST_DISK );
      i = perfstat_disk( &name, p, sizeof( perfstat_disk_t ), count );

      if (i == -1)
      {
         perror( "perfstat_disk(a)" );
         exit( 4 );
      }


/* allocate the proper data structures */

      aixdisks = malloc( count * sizeof( aixdisk_t ) );
      if (! aixdisks)
         return( -1 );

      for (i = 0;  i < count;  i++)
      {
         aixdisks[i].enabled = TRUE;
         aixdisks[i].threshold = MIN_THRESHOLD;

         strcpy( aixdisks[i].devName, p[i].name );
      }
   }
   else
      return( 0 );

#ifdef DEBUG
for (i = 0;  i < count;  i++)
   fprintf( stderr, "name = >%s<\n", aixdisks[i].devName );
fflush( stderr );
#endif


/* return the number of found AIX disks */
   return( count );
}


#define NONZERO(x) ((x)?(x):1)


static void
read_disk( int devIndex, double delta_t, double now )
{
   perfstat_disk_t d;
   perfstat_id_t id;
   long long delta, dx1, dx2;
   int count;
   static int nCPUs = 0;


/* get the number of CPUs */
   nCPUs = perfstat_cpu( NULL, NULL, sizeof( perfstat_cpu_t ), 0 );


#ifdef DEBUG
fprintf( stderr, "\n" );
fprintf( stderr, "devIndex = %d, now = %f, last_read = %f, delta_t = %f\n",
                 devIndex,
                 now,
                 aixdisks[devIndex].last_read,
                 delta_t );
fflush( stderr );
#endif


/* try to read the specified disk */

   strcpy( id.name , aixdisks[devIndex].devName );

   count = perfstat_disk( &id, &d, sizeof( perfstat_disk_t ), 1 );

   if (count == 1)
   {
      aixdisk_size[devIndex].curr_value = d.size * 1024.0 * 1024.0;

      aixdisk_free[devIndex].curr_value = d.free * 1024.0 * 1024.0;

      aixdisk_bsize[devIndex].curr_value = d.bsize;

      aixdisk_xrate[devIndex].curr_value = d.xrate * 1024.0;


      delta = d.xfers - aixdisk_xfers[devIndex].last_total_value;
      if (delta < 0LL)
         aixdisk_xfers[devIndex].curr_value = aixdisk_xfers[devIndex].last_value;
      else
         aixdisk_xfers[devIndex].curr_value = delta / delta_t;
      aixdisk_xfers[devIndex].last_value = aixdisk_xfers[devIndex].curr_value;



      delta = d.wblks - aixdisk_wbytes[devIndex].last_total_value;
      if (delta < 0LL)
         aixdisk_wbytes[devIndex].curr_value = aixdisk_wbytes[devIndex].last_value;
      else
         aixdisk_wbytes[devIndex].curr_value = (delta / delta_t) * d.bsize;
      aixdisk_wbytes[devIndex].last_value = aixdisk_wbytes[devIndex].curr_value;


      delta = d.rblks - aixdisk_rbytes[devIndex].last_total_value;
      if (delta < 0LL)
         aixdisk_rbytes[devIndex].curr_value = aixdisk_rbytes[devIndex].last_value;
      else
         aixdisk_rbytes[devIndex].curr_value = (delta / delta_t) * d.bsize;
      aixdisk_rbytes[devIndex].last_value = aixdisk_rbytes[devIndex].curr_value;


      aixdisk_qdepth[devIndex].curr_value = d.qdepth;

      
      delta = d.time - aixdisk_time[devIndex].last_total_value;
      if (delta < 0LL)
         aixdisk_time[devIndex].curr_value = aixdisk_time[devIndex].last_value;
      else
         aixdisk_time[devIndex].curr_value = (double) delta / delta_t;
      aixdisk_time[devIndex].last_value = aixdisk_time[devIndex].curr_value;


#ifdef _AIX53
      delta = d.q_full - aixdisk_q_full[devIndex].last_total_value;
      if (delta < 0LL)
         aixdisk_q_full[devIndex].curr_value = aixdisk_q_full[devIndex].last_value;
      else
         aixdisk_q_full[devIndex].curr_value = delta;
      aixdisk_q_full[devIndex].last_value = aixdisk_q_full[devIndex].curr_value;


      delta = d.rserv - aixdisk_rserv[devIndex].last_total_value;
      if (delta < 0LL)
         aixdisk_rserv[devIndex].curr_value = aixdisk_rserv[devIndex].last_value;
      else
      {
         dx2 = d.xrate - aixdisk_xrate[devIndex].last_total_value;
         aixdisk_rserv[devIndex].curr_value = HWTICS2MSECS( delta ) / NONZERO( dx2 );
      }
      aixdisk_rserv[devIndex].last_value = aixdisk_rserv[devIndex].curr_value;


      aixdisk_rtimeout[devIndex].curr_value = d.rtimeout;


      aixdisk_rfailed[devIndex].curr_value = d.rfailed;


      aixdisk_min_rserv[devIndex].curr_value = HWTICS2MSECS( d.min_rserv );


      aixdisk_max_rserv[devIndex].curr_value = HWTICS2MSECS( d.max_rserv );


      delta = d.wserv - aixdisk_wserv[devIndex].last_total_value;
      if (delta < 0LL)
         aixdisk_wserv[devIndex].curr_value = aixdisk_wserv[devIndex].last_value;
      else
      {
         dx1 = d.xfers - aixdisk_xfers[devIndex].last_total_value;
         dx2 = d.xrate - aixdisk_xrate[devIndex].last_total_value;
         aixdisk_wserv[devIndex].curr_value = HWTICS2MSECS( delta ) / NONZERO( dx1 - dx2 );
      }
      aixdisk_wserv[devIndex].last_value = aixdisk_q_full[devIndex].curr_value;


      aixdisk_wtimeout[devIndex].curr_value = d.wtimeout;


      aixdisk_wfailed[devIndex].curr_value = d.wfailed;


      aixdisk_min_wserv[devIndex].curr_value = HWTICS2MSECS( d.min_wserv );


      aixdisk_max_wserv[devIndex].curr_value = HWTICS2MSECS( d.max_wserv );


      aixdisk_wq_depth[devIndex].curr_value = d.wq_depth;


      delta = d.wq_sampled - aixdisk_wq_sampled[devIndex].last_total_value;
      if (delta < 0LL)
         aixdisk_wq_sampled[devIndex].curr_value = aixdisk_wq_sampled[devIndex].last_value;
      else
         aixdisk_wq_sampled[devIndex].curr_value = (double) delta / (100.0 * delta_t * nCPUs);
      aixdisk_wq_sampled[devIndex].last_value = aixdisk_wq_sampled[devIndex].curr_value;


      delta = d.wq_time - aixdisk_wq_time[devIndex].last_total_value;
      if (delta < 0LL)
         aixdisk_wq_time[devIndex].curr_value = aixdisk_wq_time[devIndex].last_value;
      else
      {
         dx1 = d.xfers - aixdisk_xfers[devIndex].last_total_value;
         aixdisk_wq_time[devIndex].curr_value = HWTICS2MSECS( delta )
                                                  / NONZERO( dx1 )
                                                  / delta_t;
      }
      aixdisk_wq_time[devIndex].last_value = aixdisk_wq_time[devIndex].curr_value;


      aixdisk_wq_min_time[devIndex].curr_value = HWTICS2MSECS( d.wq_min_time );


      aixdisk_wq_max_time[devIndex].curr_value = HWTICS2MSECS( d.wq_max_time );
#endif
   }


/* save values for next call */
   aixdisk_xfers[devIndex].last_total_value = d.xfers;
   aixdisk_wbytes[devIndex].last_total_value = d.wblks;
   aixdisk_rbytes[devIndex].last_total_value = d.rblks;
   aixdisk_time[devIndex].last_total_value = d.time;
#ifdef _AIX53
   aixdisk_q_full[devIndex].last_total_value = d.q_full;
   aixdisk_rserv[devIndex].last_total_value = d.rserv;
   aixdisk_wserv[devIndex].last_total_value = d.wserv;
   aixdisk_wq_sampled[devIndex].last_total_value = d.wq_sampled;
   aixdisk_wq_time[devIndex].last_total_value = d.wq_time;
#endif


#ifdef DEBUG
fprintf( stderr, "============== disk ( %s ) BEGIN ========================\n", 
                 aixdisks[devIndex].devName );
fprintf( stderr, "size        = %.0f\n", aixdisk_size[devIndex].curr_value );
fprintf( stderr, "free        = %.0f\n", aixdisk_free[devIndex].curr_value );
fprintf( stderr, "bsize       = %.0f\n", aixdisk_bsize[devIndex].curr_value );
fprintf( stderr, "xrate       = %.3f\n", aixdisk_xrate[devIndex].curr_value );
fprintf( stderr, "xfers       = %.3f\n", aixdisk_xfers[devIndex].curr_value );
fprintf( stderr, "wbytes      = %.3f\n", aixdisk_wbytes[devIndex].curr_value );
fprintf( stderr, "rbytes      = %.3f\n", aixdisk_rbytes[devIndex].curr_value );
fprintf( stderr, "qdepth      = %.0f\n", aixdisk_qdepth[devIndex].curr_value );
fprintf( stderr, "time        = %f\n", aixdisk_time[devIndex].curr_value );
#ifdef _AIX53
fprintf( stderr, "q_full      = %f\n", aixdisk_q_full[devIndex].curr_value );
fprintf( stderr, "rserv       = %f\n", aixdisk_rserv[devIndex].curr_value );
fprintf( stderr, "rtimeout    = %f\n", aixdisk_rtimeout[devIndex].curr_value );
fprintf( stderr, "rfailed     = %f\n", aixdisk_rfailed[devIndex].curr_value );
fprintf( stderr, "min_rserv   = %f\n", aixdisk_min_rserv[devIndex].curr_value );
fprintf( stderr, "max_rserv   = %f\n", aixdisk_max_rserv[devIndex].curr_value );
fprintf( stderr, "wserv       = %f\n", aixdisk_wserv[devIndex].curr_value );
fprintf( stderr, "wtimeout    = %f\n", aixdisk_wtimeout[devIndex].curr_value );
fprintf( stderr, "wfailed     = %f\n", aixdisk_wfailed[devIndex].curr_value );
fprintf( stderr, "min_wserv   = %f\n", aixdisk_min_wserv[devIndex].curr_value );
fprintf( stderr, "max_wserv   = %f\n", aixdisk_max_wserv[devIndex].curr_value );
fprintf( stderr, "wq_depth    = %f\n", aixdisk_wq_depth[devIndex].curr_value );
fprintf( stderr, "wq_sampled  = %f\n", aixdisk_wq_sampled[devIndex].curr_value );
fprintf( stderr, "wq_time     = %f\n", aixdisk_wq_time[devIndex].curr_value );
fprintf( stderr, "wq_min_time = %f\n", aixdisk_wq_min_time[devIndex].curr_value );
fprintf( stderr, "wq_max_time = %f\n", aixdisk_wq_max_time[devIndex].curr_value );
#endif
fprintf( stderr, "============== disk ( %s ) END ========================\n",
                 aixdisks[devIndex].devName );
fprintf( stderr, "\n" );
fflush( stderr );
#endif

   aixdisks[devIndex].last_read = now;
}



static double
get_current_time( void )
{
   struct timeval timeValue;
   struct timezone timeZone;


   gettimeofday( &timeValue, &timeZone );

   return( (double) (timeValue.tv_sec - boottime) + (timeValue.tv_usec / 1000000.0) );
}



static double
time_diff( int aixdisk_index, double *now )
{
   *now = get_current_time();

   return( *now - aixdisks[aixdisk_index].last_read );
}



static g_val_t
aixdisk_size_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_size[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_size_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_free_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_free[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_free_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_bsize_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_bsize[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_bsize_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}



static g_val_t
aixdisk_xrate_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_xrate[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_xrate_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_xfers_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_xfers[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_xfers_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_wbytes_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_wbytes[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_wbytes_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_rbytes_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_rbytes[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_rbytes_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_qdepth_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_qdepth[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_qdepth_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_time_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_time[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_time_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


/* The following disk properties are only available with AIX 5.3 and higher */
#ifdef _AIX53

static g_val_t
aixdisk_q_full_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_q_full[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_q_full_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_rserv_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_rserv[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_rserv_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_rtimeout_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_rtimeout[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_rtimeout_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_rfailed_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_rfailed[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_rfailed_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_min_rserv_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_min_rserv[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_min_rserv_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_max_rserv_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_max_rserv[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_max_rserv_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_wserv_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_wserv[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_wserv_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_wtimeout_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_wtimeout[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_wtimeout_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_wfailed_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_wfailed[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_wfailed_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_min_wserv_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_min_wserv[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_min_wserv_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_max_wserv_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_max_wserv[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_max_wserv_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_wq_depth_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_wq_depth[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_wq_depth_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_wq_sampled_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_wq_sampled[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_wq_sampled_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_wq_time_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_wq_time[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_wq_time_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_wq_min_time_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_wq_min_time[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_wq_min_time_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}


static g_val_t
aixdisk_wq_max_time_func( int aixdisk_index )
{
   double delta_t,
          now;
   g_val_t val;


   if (aixdisks[aixdisk_index].enabled)
   {
      delta_t = time_diff( aixdisk_index, &now );

      if (delta_t > aixdisks[aixdisk_index].threshold)
         read_disk( aixdisk_index, delta_t, now );

      val.d = aixdisk_wq_max_time[aixdisk_index].curr_value;
   }
   else
      val.d = -1.0;

#ifdef DEBUG
fprintf( stderr, "aixdisk_wq_max_time_func = %f\n", val.d ); fflush( stderr );
#endif


   return( val );
}

#endif


/* Initialize the given metric by allocating the per metric data
   structure and inserting a metric definition for each network
   interface found.
*/
static aixdisk_data_t *init_metric( apr_pool_t *p,
                                    apr_array_header_t *ar,
                                    int aixdisk_count,
                                    char *name,
                                    char *desc,
                                    char *units )
{
   int i;
   Ganglia_25metric *gmi;
   aixdisk_data_t *aix_hdisk;


   aix_hdisk = apr_pcalloc( p, sizeof( aixdisk_data_t ) * aixdisk_count );

   for (i = 0;  i < aixdisk_count;  i++)
   {
      gmi = apr_array_push( ar );

      /* gmi->key will be automatically assigned by gmond */
      gmi->name = apr_psprintf( p, "%s_%s", aixdisks[i].devName, name );
      gmi->tmax = 60;
      gmi->type = GANGLIA_VALUE_DOUBLE;
      gmi->units = apr_pstrdup( p, units );
      gmi->slope = apr_pstrdup( p, "both" );
      gmi->fmt = apr_pstrdup( p, "%.1f" );
      gmi->msg_size = UDP_HEADER_SIZE + 16;
      gmi->desc = apr_psprintf( p, "%s %s", aixdisks[i].devName, desc );
   }

   return( aix_hdisk);
}



/*
 * Declare ourselves so the configuration routines can find and know us.
 * We'll fill it in at the end of the module.
 */
extern mmodule aixdisk_module;


static int aixdisk_metric_init( apr_pool_t *p )
{
   int i;
   double now;
   Ganglia_25metric *gmi;
   struct vario var;


/* Enable collection of disk input and output statistics in AIX */

   sys_parm( SYSP_GET, SYSP_V_IOSTRUN, &var );
   allowDiskPerfCollection = var.v.v_iostrun.value;

   var.v.v_iostrun.value = 1; /* 1 to set & 0 to unset */
   sys_parm( SYSP_SET, SYSP_V_IOSTRUN, &var );


/* Initialize all required data and structures */

   aixdisk_count = detect_aixdisk_devices();


/* Allocate a pool that will be used by this module */
   apr_pool_create( &pool, p );

   metric_info = apr_array_make( pool, 2, sizeof( Ganglia_25metric ) );


/* Initialize each metric */
   aixdisk_size = init_metric( pool,
                               metric_info,
                               aixdisk_count,
                               "size",
                               "total disk size",
                               "bytes" );
   aixdisk_free = init_metric( pool,
                               metric_info,
                               aixdisk_count,
                               "free",
                               "free disk size",
                               "bytes" );
   aixdisk_bsize = init_metric( pool,
                                metric_info,
                                aixdisk_count,
                                "bsize",
                                "block size",
                                "bytes" );
   aixdisk_xrate = init_metric( pool,
                                metric_info,
                                aixdisk_count,
                                "xrate",
                                "transfer rate capability",
                                "bytes/sec" );
   aixdisk_xfers = init_metric( pool,
                                metric_info,
                                aixdisk_count,
                                "xfers",
                                "number of transfers to/from disk",
                                "transfers/sec" );
   aixdisk_wbytes = init_metric( pool,
                                 metric_info,
                                 aixdisk_count,
                                 "wbytes",
                                 "number of bytes written to disk",
                                 "bytes" );
   aixdisk_rbytes = init_metric( pool,
                                 metric_info,
                                 aixdisk_count,
                                 "rbytes",
                                 "number of bytes read from disk",
                                 "bytes" );
   aixdisk_qdepth = init_metric( pool,
                                 metric_info,
                                 aixdisk_count,
                                 "qdepth",
                                 "instantaneous service queue depth",
                                 "" );
   aixdisk_time = init_metric( pool,
                               metric_info,
                               aixdisk_count,
                               "time",
                               "percentage of time disk is active",
                               "" );
#ifdef _AIX53
   aixdisk_q_full = init_metric( pool,
                                 metric_info,
                                 aixdisk_count,
                                 "q_full",
                                 "service queue full occurrence count",
                                 "" );
   aixdisk_rserv = init_metric( pool,
                                metric_info,
                                aixdisk_count,
                                "rserv",
                                "read or receive service time",
                                "" );
   aixdisk_rtimeout = init_metric( pool,
                                   metric_info,
                                   aixdisk_count,
                                   "rtimeout",
                                   "number of read request timeouts",
                                   "" );
   aixdisk_rfailed = init_metric( pool,
                                  metric_info,
                                  aixdisk_count,
                                  "rfailed",
                                  "number of failed read requests",
                                  "" );
   aixdisk_min_rserv = init_metric( pool,
                                    metric_info,
                                    aixdisk_count,
                                    "min_rserv",
                                    "minimum read or receive service time",
                                    "" );
   aixdisk_max_rserv = init_metric( pool,
                                    metric_info,
                                    aixdisk_count,
                                    "max_rserv",
                                    "maximum read or receive service time",
                                    "" );
   aixdisk_wserv = init_metric( pool,
                                metric_info,
                                aixdisk_count,
                                "wserv",
                                "write or send service time",
                                "" );
   aixdisk_wtimeout = init_metric( pool,
                                   metric_info,
                                   aixdisk_count,
                                   "wtimeout",
                                   "number of write request timeouts",
                                   "" );
   aixdisk_wfailed = init_metric( pool,
                                  metric_info,
                                  aixdisk_count,
                                  "wfailed",
                                  "number of failed write requests",
                                  "" );
   aixdisk_min_wserv = init_metric( pool,
                                    metric_info,
                                    aixdisk_count,
                                    "min_wserv",
                                    "minimum write or send service time",
                                    "" );
   aixdisk_max_wserv = init_metric( pool,
                                    metric_info,
                                    aixdisk_count,
                                    "max_wserv",
                                    "maximum write or send service time",
                                    "" );
   aixdisk_wq_depth = init_metric( pool,
                                   metric_info,
                                   aixdisk_count,
                                   "wq_depth",
                                   "instantaneous wait queue depth",
                                   "" );
   aixdisk_wq_sampled = init_metric( pool,
                                     metric_info,
                                     aixdisk_count,
                                     "wq_sampled",
                                     "accumulated sampled dk_wq_depth",
                                     "" );
   aixdisk_wq_time = init_metric( pool,
                                  metric_info,
                                  aixdisk_count,
                                  "wq_time",
                                  "accumulated wait queueing time",
                                  "" );
   aixdisk_wq_min_time = init_metric( pool,
                                      metric_info,
                                      aixdisk_count,
                                      "wq_min_time",
                                      "minimum wait queueing time",
                                      "" );
   aixdisk_wq_max_time = init_metric( pool,
                                      metric_info,
                                      aixdisk_count,
                                      "wq_max_time",
                                      "maximum wait queueing time",
                                      "" );
#endif


/* Add a terminator to the array and replace the empty static metric definition
   array with the dynamic array that we just created
*/
   gmi = apr_array_push( metric_info );
   memset( gmi, 0, sizeof( *gmi ));

   aixdisk_module.metrics_info = (Ganglia_25metric *) metric_info->elts;


#ifndef STAND_ALONE
   for (i = 0;  aixdisk_module.metrics_info[i].name != NULL;  i++)
   {
      /* Initialize the metadata storage for each of the metrics and then
       *  store one or more key/value pairs.  The define MGROUPS defines
       *  the key for the grouping attribute. */
      MMETRIC_INIT_METADATA( &(aixdisk_module.metrics_info[i]), p );
      MMETRIC_ADD_METADATA( &(aixdisk_module.metrics_info[i]), MGROUP, "aixdisk" );
   }
#endif


/* initialize the routines which require a time interval */

   boottime = boottime_func_CALLED_ONCE();
   now = get_current_time();
   for (i = 0;  i < aixdisk_count;  i++)
   {
      aixdisk_size[i].curr_value = aixdisk_size[i].last_value = 0.0;
      aixdisk_free[i].curr_value = aixdisk_free[i].last_value = 0.0;
      aixdisk_bsize[i].curr_value = aixdisk_bsize[i].last_value = 0.0;
      aixdisk_xrate[i].curr_value = aixdisk_xrate[i].last_value = 0.0;
      aixdisk_xfers[i].curr_value = aixdisk_xfers[i].last_value = 0.0;
      aixdisk_wbytes[i].curr_value = aixdisk_wbytes[i].last_value = 0.0;
      aixdisk_rbytes[i].curr_value = aixdisk_rbytes[i].last_value = 0.0;
      aixdisk_qdepth[i].curr_value = aixdisk_qdepth[i].last_value = 0.0;
      aixdisk_time[i].curr_value = aixdisk_time[i].last_value = 0.0;
#ifdef _AIX53
      aixdisk_q_full[i].curr_value = aixdisk_q_full[i].last_value = 0.0;
      aixdisk_rserv[i].curr_value = aixdisk_rserv[i].last_value = 0.0;
      aixdisk_rtimeout[i].curr_value = aixdisk_rtimeout[i].last_value = 0.0;
      aixdisk_rfailed[i].curr_value = aixdisk_rfailed[i].last_value = 0.0;
      aixdisk_min_rserv[i].curr_value = aixdisk_min_rserv[i].last_value = 0.0;
      aixdisk_max_rserv[i].curr_value = aixdisk_max_rserv[i].last_value = 0.0;
      aixdisk_wserv[i].curr_value = aixdisk_wserv[i].last_value = 0.0;
      aixdisk_wtimeout[i].curr_value = aixdisk_wtimeout[i].last_value = 0.0;
      aixdisk_wfailed[i].curr_value = aixdisk_wfailed[i].last_value = 0.0;
      aixdisk_min_wserv[i].curr_value = aixdisk_min_wserv[i].last_value = 0.0;
      aixdisk_max_wserv[i].curr_value = aixdisk_max_wserv[i].last_value = 0.0;
      aixdisk_wq_depth[i].curr_value = aixdisk_wq_depth[i].last_value = 0.0;
      aixdisk_wq_sampled[i].curr_value = aixdisk_wq_sampled[i].last_value = 0.0;
      aixdisk_wq_time[i].curr_value = aixdisk_wq_time[i].last_value = 0.0;
      aixdisk_wq_min_time[i].curr_value = aixdisk_wq_min_time[i].last_value = 0.0;
      aixdisk_wq_max_time[i].curr_value = aixdisk_wq_max_time[i].last_value = 0.0;

      read_disk( i, 1.0, now );

      sleep( 1 );
      now += 1.0;

      read_disk( i, 1.0, now );
#endif
   }


/* return OK */
   return( 0 );
}



static void aixdisk_metric_cleanup ( void )
{
   struct vario var;


/* set old value again */
   var.v.v_iostrun.value = allowDiskPerfCollection;
   sys_parm( SYSP_SET, SYSP_V_IOSTRUN, &var );
}



static g_val_t aixdisk_metric_handler ( int metric_index )
{
   g_val_t val;
   char *p,
         name[256];
   int i, devIndex; 


/* Get the metric name and device index from the combined name that was
 * passed in
 */
   strcpy( name, aixdisk_module.metrics_info[metric_index].name );

   p = index( name, '_' ) + 1;
   name[p - name - 1] = '\0';


/* now we need to match the name with the name of all found disk devices */
   devIndex = -1;

   for (i = 0;  i < aixdisk_count;  i++)
     if (strcmp( name, aixdisks[i].devName ) == 0)
     {
        devIndex = i;
        break;
     }

   if (devIndex == -1)
   {
      val.uint32 = 0; /* default fallback */
      return( val );
   }


/* jump into the right function */

   if (strcmp( p, "size" ) == 0)
      return( aixdisk_size_func( devIndex ) );

   if (strcmp( p, "free" ) == 0)
      return( aixdisk_free_func( devIndex ) );

   if (strcmp( p, "bsize" ) == 0)
      return( aixdisk_bsize_func( devIndex ) );

   if (strcmp( p, "xrate" ) == 0)
      return( aixdisk_xrate_func( devIndex ) );

   if (strcmp( p, "xfers" ) == 0)
      return( aixdisk_xfers_func( devIndex ) );

   if (strcmp( p, "xrate" ) == 0)
      return( aixdisk_xrate_func( devIndex ) );

   if (strcmp( p, "wbytes" ) == 0)
      return( aixdisk_wbytes_func( devIndex ) );

   if (strcmp( p, "rbytes" ) == 0)
      return( aixdisk_rbytes_func( devIndex ) );

   if (strcmp( p, "qdepth" ) == 0)
      return( aixdisk_qdepth_func( devIndex ) );

   if (strcmp( p, "time" ) == 0)
      return( aixdisk_time_func( devIndex ) );

#ifdef _AIX53
   if (strcmp( p, "q_full" ) == 0)
      return( aixdisk_q_full_func( devIndex ) );

   if (strcmp( p, "rserv" ) == 0)
      return( aixdisk_rserv_func( devIndex ) );

   if (strcmp( p, "rtimeout" ) == 0)
      return( aixdisk_rtimeout_func( devIndex ) );

   if (strcmp( p, "rfailed" ) == 0)
      return( aixdisk_rfailed_func( devIndex ) );

   if (strcmp( p, "min_rserv" ) == 0)
      return( aixdisk_min_rserv_func( devIndex ) );

   if (strcmp( p, "max_rserv" ) == 0)
      return( aixdisk_max_rserv_func( devIndex ) );

   if (strcmp( p, "wserv" ) == 0)
      return( aixdisk_wserv_func( devIndex ) );

   if (strcmp( p, "wtimeout" ) == 0)
      return( aixdisk_wtimeout_func( devIndex ) );

   if (strcmp( p, "wfailed" ) == 0)
      return( aixdisk_wfailed_func( devIndex ) );

   if (strcmp( p, "min_wserv" ) == 0)
      return( aixdisk_min_wserv_func( devIndex ) );

   if (strcmp( p, "max_wserv" ) == 0)
      return( aixdisk_max_wserv_func( devIndex ) );

   if (strcmp( p, "wq_depth" ) == 0)
      return( aixdisk_wq_depth_func( devIndex ) );

   if (strcmp( p, "wq_sampled" ) == 0)
      return( aixdisk_wq_sampled_func( devIndex ) );

   if (strcmp( p, "wq_time" ) == 0)
      return( aixdisk_wq_time_func( devIndex ) );

   if (strcmp( p, "wq_min_time" ) == 0)
      return( aixdisk_wq_min_time_func( devIndex ) );

   if (strcmp( p, "wq_max_time" ) == 0)
      return( aixdisk_wq_max_time_func( devIndex ) );
#endif

   val.uint32 = 0; /* default fallback */
   return( val );
}



mmodule aixdisk_module =
{
   STD_MMODULE_STUFF,
   aixdisk_metric_init,
   aixdisk_metric_cleanup,
   NULL, /* defined dynamically */
   aixdisk_metric_handler
};


#ifdef STAND_ALONE
/*
   compile with:

xlc_r -DSTAND_ALONE -DDEBUG -U_AIX43 -qlanglvl=extc99 -I. -I../../.. -I/opt/freeware/include/apr-1 -I../../../include -I../../../lib -I../../../libmetrics -qmaxmem=16384 -DSYSV -D_AIX -D_AIX32 -D_AIX41 -D_AIX43 -D_AIX51 -D_AIX52 -D_AIX53 -D_ALL_SOURCE -DFUNCPROTO=15 -O -I/opt/freeware/include -D_ALL_SOURCE -DAIX -DHAVE_PERFSTAT -o aixdisk_test mod_aixdisk.c -L/opt/freeware/lib -lm -ldl -lperfstat -lcfg -lodm -lnsl -lpcre -lexpat -lconfuse -lapr-1 -lpthreads -lpthread -qmaxmem=16384 -Wl,-bmaxdata:0x80000000



 */


int main( int argc, char *argv[] )
{
   int i;
   double now;
   apr_pool_t *p;


   now = get_current_time();

   aixdisk_metric_init( p );

   sleep( 2.0 ) ;
   now += 2.0;

   for (i = 0;  i < aixdisk_count;  i++)
   {
      read_disk( i, 2.0, now );
   }

   sleep( 2.0 ) ;
   now += 2.0;

   for (i = 0;  i < aixdisk_count;  i++)
   {
      read_disk( i, 2.0, now );
   }
}
#endif

