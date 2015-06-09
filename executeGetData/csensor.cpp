/*
 *  csensor.cpp
 *  qcn
 *
 *  Created by Carl Christensen on 08/11/2007.
 *  Copyright 2007 Stanford University.  All rights reserved.
 *
 * Implementation file for CSensor base classes
 * note it requires a reference to the sm shared memory datastructure (CQCNShMem)
 */

#include "csensor.h"
#include <math.h>
#include <cmath>
#include <complex>
//#include <except.h>

#include "mysql.h"
#include <pthread.h>

using namespace std;

// device ID
// TODO:set from argv
//const int device_id;
int device_id;

// local MySQL
const char *hostname = "localhost";
const char *username = "root";
const char *password = "";
const char *database = "csn";

// seismic.balog.jp MySQL
/*
const char *hostname = "";
const char *username = "";
const char *password = "";
const char *database = "";
*/

unsigned long portnumber = 3306;
char insert_q[500];

MYSQL *mysql;
MYSQL_RES *g_res = NULL;

extern double dtime();

extern CQCNShMem* sm;
bool g_bStop = false;

// config for recording trigger
const float LTA_second = 10.0f;
const float STA_second = 3.0f;

int LTA_array_numbers = (int)(LTA_second / DT); //DT is defined in define.h
int STA_array_numbers = (int)(STA_second / DT); //DT is defined in define.h
int LTA_STA_diff = LTA_array_numbers - STA_array_numbers;

double x_offset = 0.0f;
double y_offset = 0.0f;
double z_offset = -9.8f;    //Must be minus value

const float limitTimes = 3.0f;

const int triggerLimit = 4;
int triggerCount = 0;

const double recordTime = 300.0f; //second
double startRecordTime;

bool isEarthQuake = false;

CSensor::CSensor()
  :
    m_iType(SENSOR_NOTFOUND),
    m_port(-1),
    m_bSingleSampleDT(false),
    m_strSensor("")
{
}
/*
CSensor::CSensor(int d_id, double x_off, double y_off, double z_off)
  :
    m_iType(SENSOR_NOTFOUND),
    m_port(-1),
    m_bSingleSampleDT(false),
    m_strSensor("")
{
    device_id = d_id;
    x_offset = x_off;
    y_offset = y_off;
    z_offset = z_off;
}
*/
CSensor::~CSensor()
{
    if (m_port>-1) closePort();
}

bool CSensor::getSingleSampleDT()
{
   return m_bSingleSampleDT;
}

void CSensor::setSingleSampleDT(const bool bSingle)
{
   m_bSingleSampleDT = bSingle;
}

const char* CSensor::getSensorStr()
{
   return m_strSensor.c_str();
}

void CSensor::setSensorStr(const char* strIn)
{
    if (strIn)
       m_strSensor.assign(strIn);
    else
       m_strSensor.assign("");
}

void CSensor::setType(e_sensor esType)
{
   m_iType = esType;
}

void CSensor::setPort(const int iPort)
{
   m_port = iPort;
}

int CSensor::getPort()
{
   return m_port;
}

void CSensor::closePort()
{
    fprintf(stdout, "Closing port...\n");
}

const e_sensor CSensor::getTypeEnum()
{
   return m_iType;
}

const char* CSensor::getTypeStr()
{
   switch (m_iType) {
     case SENSOR_MAC_PPC_TYPE1:
        return "PPC Mac Type 1";
        break;
     case SENSOR_MAC_PPC_TYPE2:
        return "PPC Mac Type 2";
        break;
     case SENSOR_MAC_PPC_TYPE3:
        return "PPC Mac Type 3";
        break;
     case SENSOR_MAC_INTEL:
        return "Intel Mac";
        break;
     case SENSOR_WIN_THINKPAD:
        return "Lenovo Thinkpad";
        break;
     case SENSOR_WIN_HP:
        return "HP Laptop";
        break;
     case SENSOR_USB:
        return "JoyWarrior 24F8 USB";
        break;
     default:
        return "Not Found";
   }
}

// this is the heart of qcn -- it gets called 50-500 times a second!
inline bool CSensor::mean_xyz(const bool bVerbose)
{
/* This subroutine finds the mean amplitude for x,y, & z of the sudden motion
 * sensor in a window dt from time t0.
 */

	static long lLastSample = 10L;  // store last sample size, start at 10 so doesn't give less sleep below, but will if lastSample<3
	float x1,y1,z1;
	double dTimeDiff=0.0f;
	bool result = false;

	// set up pointers to array offset for ease in functions below
	float *px2, *py2, *pz2;
	double *pt2;

	if (g_bStop || !sm) throw EXCEPTION_SHUTDOWN;   // see if we're shutting down, if so throw an exception which gets caught in the sensor_thread

	px2 = (float*) &(sm->x0[sm->lOffset]);
	py2 = (float*) &(sm->y0[sm->lOffset]);
	pz2 = (float*) &(sm->z0[sm->lOffset]);
	pt2 = (double*) &(sm->t0[sm->lOffset]);
	sm->lSampleSize = 0L;

	*px2 = *py2 = *pz2 = *pt2 = 0.0f;  // zero sample averages

	// this will get executed at least once, then the time is checked to see if we have enough time left for more samples
	do {
		if (sm->lSampleSize < SAMPLE_SIZE) {  // note we only get a sample if sample size < 10
			x1 = y1 = z1 = 0.0f;
			result = read_xyz(x1, y1, z1);
			*px2 += x1;
			*py2 += y1;
			*pz2 += z1;
			sm->lSampleSize++; // only increment if not a single sample sensor
		}  // done sample size stuff

		// dt is in seconds, want to slice it into 10 (SAMPLING_FREQUENCY), put into microseconds, so multiply by 100000
		// using usleep saves all the FP math every millisecond

		// sleep for dt seconds, this is where the CPU time gets used up, for dt/10 6% CPU, for dt/1000 30% CPU!
		// note the use of the "lLastSample" -- if we are getting low sample rates i.e. due to an overworked computer,
		// let's drop the sleep time dramatically and hope it can "catch up"
		// usleep((long) lLastSample < 3 ? DT_MICROSECOND_SAMPLE/100 : DT_MICROSECOND_SAMPLE);

		usleep(DT_MICROSECOND_SAMPLE); // usually 2000, which is 2 ms or .002 seconds, 10 times finer than the .02 sec / 50 Hz sample rate
		sm->t0active = dtime(); // use the function in the util library (was used to set t0)
		dTimeDiff = sm->t0check - sm->t0active;  // t0check should be bigger than t0active by dt, when t0check = t0active we're done
	}
	while (dTimeDiff > 0.0f);

	//fprintf(stdout, "Sensor sampling info:  t0check=%f  t0active=%f  diff=%f  timeadj=%d  sample_size=%ld, dt=%f\n",
	//   sm->t0check, sm->t0active, dTimeDiff, sm->iNumReset, sm->lSampleSize, sm->dt);
	//fprintf(stdout, "sensorout,%f,%f,%f,%d,%ld,%f\n",
	//   sm->t0check, sm->t0active, dTimeDiff, sm->iNumReset, sm->lSampleSize, sm->dt);
	//fflush(stdout);

	lLastSample = sm->lSampleSize;

	// store values i.e. divide by sample size
	*px2 /= (double) sm->lSampleSize;
	*py2 /= (double) sm->lSampleSize;
	*pz2 /= (double) sm->lSampleSize;
	*pt2 = sm->t0active; // save the time into the array, this is the real clock time

	if (bVerbose) {
		// Everytime INSERT to MySQL
		sprintf(insert_q,
                    "INSERT INTO Event (device_id, t0check, t0active, x_acc, y_acc, z_acc, sample_size, offset) VALUES('%d', '%f', '%f', '%f', '%f', '%f', '%ld', '%ld')",
                      device_id, sm->t0check, *pt2, *px2, *py2, *pz2, sm->lSampleSize, sm->lOffset);
		query(insert_q);

        // To check isEarthQuake
		preserve_xyz.push_back(*new PreserveXYZ(px2, py2, pz2, pt2, &(sm->t0check), &(sm->lSampleSize), &(sm->lOffset)));

		if( preserve_xyz.size() > LTA_array_numbers ){
			preserve_xyz.erase(preserve_xyz.begin());
		}

		if(isEarthQuake) { // While a recordTime, couldn't check isEarthQuake
			if(isQuitRecording()) {
				printf("Recording quits at %f\n", sm->t0check);
				isEarthQuake = false;
			}
		} else { // Only check isEarthQuake
            isEarthQuake = isStrikeEarthQuake();
		}
	}

	// if active time is falling behind the checked (wall clock) time -- set equal, may have gone to sleep & woken up etc
	sm->t0check += sm->dt;	// t0check is the "ideal" time i.e. start time + the dt interval

	sm->ullSampleTotal += sm->lSampleSize;
	sm->ullSampleCount++;

	sm->fRealDT += fabs(sm->t0active - sm->t0check);

	if (fabs(dTimeDiff) > TIME_ERROR_SECONDS) { // if our times are different by a second, that's a big lag, so let's reset t0check to t0active
		if (bVerbose) {
			fprintf(stdout, "Timing error encountered t0check=%f  t0active=%f  diff=%f  timeadj=%d  sample_size=%ld, dt=%f, resetting...\n",
			sm->t0check, sm->t0active, dTimeDiff, sm->iNumReset, sm->lSampleSize, sm->dt);
		}

		#ifndef _DEBUG
			return false;   // if we're not debugging, this is a serious run-time problem, so reset time & counters & try again
		#endif
	}

	return true;
}

bool CSensor::isQuitRecording() {
	if((sm->t0check - startRecordTime) >= recordTime) {
		return true;
	}
	else return false;
}

bool CSensor::isStrikeEarthQuake()
{
	float LTA_z = 0.0f, STA_z = 0.0f;
	double LTA_average = 0.0f, STA_average = 0.0f;

	if(preserve_xyz.size() == LTA_array_numbers)
	{
		for(int i = preserve_xyz.size()-1; i >= 0; i--)
		{
			LTA_z += fabs(preserve_xyz[i].tmp_z - z_offset);

			if(i == LTA_STA_diff)
				STA_z = LTA_z;
		}

		LTA_average = LTA_z / (double)LTA_array_numbers;
		STA_average = STA_z / (double)STA_array_numbers;

		//debug
		//if(fabs(LTA_average - STA_average) > 0.002) {
			//fprintf(stdout, "%f %f %f %f %f\n\n", LTA_z, STA_z, LTA_average, STA_average, (LTA_average - STA_average));
		//}

		if( fabs(LTA_average * limitTimes) < fabs(STA_average) ) {
			triggerCount++;

			//fprintf(stdout, "%f %f %f %f %f %d\n\n", LTA_z, STA_z, LTA_average, STA_average, (LTA_average - STA_average), triggerCount);
			if( triggerCount == triggerLimit ){
				triggerCount = 0;
				startRecordTime = preserve_xyz.back().tmp_id_t;
				printf("Recording starts at %f\n", startRecordTime);	//for logging
				return true;
			}
			else return false;
		} else {
			triggerCount = 0;
			return false;
		}
	}
	else
		return false;
}

int CSensor::connectDatabase(){
  mysql = mysql_init(NULL);
  if (NULL == mysql){
    printf("error: %sn", mysql_error(mysql));
    return -1;
  }
  if (NULL == mysql_real_connect(mysql, hostname, username, password, database, portnumber, NULL, 0)){
  // error
  printf("error: %sn", mysql_error(mysql));
  return -1;
} else {
  // success
}
  return 1;
}
void CSensor::closeDatabase(){
  if(g_res){
    //freeResult(g_res);
    g_res = NULL;
  }
  mysql_close(mysql);
}

MYSQL_RES *CSensor::query(char *sql_string){
  if(g_res){
    freeResult(g_res);
    g_res = NULL;
  }

  if (mysql_query(mysql, sql_string)) {
    printf("error: %sn", mysql_error(mysql));
    return NULL;
  }

  g_res = mysql_use_result(mysql);
  return g_res;
}

MYSQL_ROW CSensor::fetchRow(MYSQL_RES *res){
  return mysql_fetch_row(res);
}

void CSensor::freeResult(MYSQL_RES *res){
  mysql_free_result(res);
  g_res = NULL;
}

//---------------------------------------------------------------------------
/*
void CSensor::fourier ( double x[], complex<double> comp[], double dt, int n, int nn )
{
const complex<double> zero(0.,0.);
int i, nyquist;
double f,y,f1,f2,f3;
	                              // FFTの準備
    for ( i=0; i<n; i++ )
        comp[i] = complex<double>( x[i], 0. ) ;
    for ( i=n; i<nn; i++ )
        comp[i] = zero ;
                              // フーリエ変換で加速度のフーリエスペクトル
    StatusBar1->SimpleText = "フーリエ変換" ;
    StatusBar1->Refresh() ;
    fast  ( comp, nn, -1 ) ;
    for ( i=0; i<nn; i++ ) comp[i] /= (double)nn;
                              // フィルターをかける
    StatusBar1->SimpleText = "フィルター操作" ;
    StatusBar1->Refresh() ;

    nyquist=nn/2;
    comp[0]=zero;

    for ( i=1; i<=nyquist; i++ ) {
        f=(double)i/(double)nn/dt;
        y=f/10.;
        f1=sqrt(1./f);
        f2=1./sqrt(1.+0.694*y*y+0.241*pow(y,4)+0.0557*pow(y,6)
                +0.009664*pow(y,8)+0.00134*pow(y,10)+0.000155*pow(y,12));
        f3=sqrt(1.-exp(-(pow(f/0.5,3))));
        comp[i]=f1*f2*f3*comp[i];
    }
    for ( i=nyquist+1; i<nn; i++ ) {
        f=(double)(nn-i)/(double)nn/dt;
        y=f/10.;
        f1=sqrt(1./f);
        f2=1./sqrt(1.+0.694*y*y+0.241*pow(y,4)+0.0557*pow(y,6)
                +0.009664*pow(y,8)+0.00134*pow(y,10)+0.000155*pow(y,12));
        f3=sqrt(1.-exp(-(pow(f/0.5,3))));
        comp[i]=f1*f2*f3*comp[i];
    }
                              // フーリエ逆変換で時刻歴波形に戻す
    StatusBar1->SimpleText = "フーリエ逆変換" ;
    StatusBar1->Refresh() ;
    fast ( comp, nn, +1 ) ;
    for ( i=0; i<n; i++ ) {
        x[i] = real( comp[i] ) ;
    }
}
//---------------------------------------------------------------------------
// 高速フーリエ変換
//      参考文献：大崎順彦「新・地震動のスペクトル解析入門」鹿島出版会

void CSensor::fast ( complex<double> x[], int nn, int ind )
{
complex<double> temp, theta;
int i, j, k, m, kmax, istep, npower;
static const double PI = 6*asin( 0.5 ) ;

    npower = log( (double) nn + 1 ) / log( 2. ) ;
    for ( i=1; i<nn-1; i++ ) {
        m = 1 ;
        j = 0 ;
        for ( k=0; k<npower; k++ ) {
            j += (( i & m ) >> k) << ( npower - k - 1 ) ;
            m *= 2 ;
        }
        if ( i < j ) {
            temp = x[j] ;
            x[j] = x[i] ;
            x[i] = temp ;
        }
    }
    kmax = 1 ;
    while ( kmax<nn ) {
        istep = kmax * 2 ;
        for ( k=0; k<kmax; k++ ) {
            theta = complex<double>( 0., PI * ind * k / kmax ) ;
            for ( i=k; i<nn; i=i+istep ) {
                j = i + kmax ;
                temp = x[j] * exp( theta ) ;
                x[j] = x[i] - temp ;
                x[i] = x[i] + temp ;
            }
        }
        kmax = istep ;
    }
}
*/
