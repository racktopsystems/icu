/*
**********************************************************************
* Copyright (c) 2003, International Business Machines
* Corporation and others.  All Rights Reserved.
**********************************************************************
* Author: Alan Liu
* Created: July 21 2003
* Since: ICU 2.8
**********************************************************************
*/

#if !UCONFIG_NO_FORMATTING

#include "olsontz.h"
#include "unicode/ures.h"
#include "unicode/simpletz.h"
#include "unicode/gregocal.h"
#include "cmemory.h"
#include "uassert.h"

U_NAMESPACE_BEGIN

#define SECONDS_PER_DAY (24*60*60)

static const int32_t ZEROS[] = {0,0};

const char OlsonTimeZone::fgClassID = 0; // Value is irrelevant

//----------------------------------------------------------------------
// Support methods

// TODO clean up; consolidate with GregorianCalendar, TimeZone, StandarTimeZone

static int32_t floorDivide(int32_t numerator, int32_t denominator) {
    return (numerator >= 0) ?
        numerator / denominator : ((numerator + 1) / denominator) - 1;
}

static double floorDivide(double numerator, double denominator,
                          double& remainder) {
    double quotient = uprv_floor(numerator / denominator);
    remainder = numerator - (quotient * denominator);
    return quotient;
}

static int32_t floorDivide(double numerator, int32_t denominator,
                           int32_t& remainder) {
    double quotient, rem;
    quotient = floorDivide(numerator, denominator, rem);
    remainder = (int32_t) rem;
    return (int32_t) quotient;
}

static const int32_t JULIAN_1_CE    = 1721426; // January 1, 1 CE Gregorian
static const int32_t JULIAN_1970_CE = 2440588; // January 1, 1970 CE Gregorian

static const int16_t DAYS_BEFORE[] =
    {0,31,59,90,120,151,181,212,243,273,304,334,
     0,31,60,91,121,152,182,213,244,274,305,335};

static const int8_t MONTH_LENGTH[] =
    {31,28,31,30,31,30,31,31,30,31,30,31,
     31,29,31,30,31,30,31,31,30,31,30,31};

static UBool isLeapYear(int year) {
    return (year%4 == 0) && ((year%100 != 0) || (year%400 == 0));
}

/**
 * Convert a year, month, and day-of-month, given in the proleptic Gregorian
 * calendar, to 1970 epoch days.
 * @param year Gregorian year, with 0 == 1 BCE, -1 == 2 BCE, etc.
 * @param month 0-based month, with 0==Jan
 * @param dom 1-based day of month
 */
static double fieldsToDay(int32_t year, int32_t month, int32_t dom) {

    int32_t y = year - 1;

    double julian = 365 * y + floorDivide(y, 4) + (JULIAN_1_CE - 3) + // Julian cal
        floorDivide(y, 400) - floorDivide(y, 100) + 2 + // => Gregorian cal
        DAYS_BEFORE[month + (isLeapYear(year) ? 12 : 0)] + dom; // => month/dom

    return julian - JULIAN_1970_CE; // JD => epoch day
}

/**
 * Convert a 1970-epoch day number to proleptic Gregorian year, month,
 * day-of-month, and day-of-week.
 * @param day 1970-epoch day (integral value)
 * @param year output parameter to receive year
 * @param month output parameter to receive month (0-based, 0==Jan)
 * @param dom output parameter to receive day-of-month (1-based)
 * @param dow output parameter to receive day-of-week (1-based, 1==Sun)
 */
static void dayToFields(double day, int32_t& year, int32_t& month,
                        int32_t& dom, int32_t& dow) {
    int32_t doy;

    // Convert from 1970 CE epoch to 1 CE epoch (Gregorian calendar)
    day += JULIAN_1970_CE - JULIAN_1_CE;

    int32_t n400 = floorDivide(day, 146097, doy); // 400-year cycle length
    int32_t n100 = floorDivide(doy, 36524, doy); // 100-year cycle length
    int32_t n4   = floorDivide(doy, 1461, doy); // 4-year cycle length
    int32_t n1   = floorDivide(doy, 365, doy);
    year = 400*n400 + 100*n100 + 4*n4 + n1;
    if (n100 == 4 || n1 == 4) {
        doy = 365; // Dec 31 at end of 4- or 400-year cycle
    } else {
        ++year;
    }
    
    UBool isLeap = isLeapYear(year);
    
    // Gregorian day zero is a Monday.
    dow = (int32_t) uprv_fmod(day + 1, 7);
    dow += (dow < 0) ? (UCAL_SUNDAY + 7) : UCAL_SUNDAY;

    // Common Julian/Gregorian calculation
    int32_t correction = 0;
    int32_t march1 = isLeap ? 60 : 59; // zero-based DOY for March 1
    if (doy >= march1) {
        correction = isLeap ? 1 : 2;
    }
    month = (12 * (doy + correction) + 6) / 367; // zero-based month
    dom = doy - DAYS_BEFORE[month + (isLeap ? 12 : 0)] + 1; // one-based DOM
}

//----------------------------------------------------------------------

/**
 * Default constructor.  Creates a time zone with an empty ID and
 * a fixed GMT offset of zero.
 */
OlsonTimeZone::OlsonTimeZone() : finalZone(0), finalYear(INT32_MAX) {
    constructEmpty();
}

/**
 * Construct a GMT+0 zone with no transitions.  This is done when a
 * constructor fails so the resultant object is well-behaved.
 */
void OlsonTimeZone::constructEmpty() {
    transitionCount = 0;
    typeCount = 1;
    transitionTimes = typeOffsets = ZEROS;
    typeData = (const uint8_t*) ZEROS;
}

/**
 * Construct from a resource bundle
 * @param top the top-level zoneinfo resource bundle.  This is used
 * to lookup the rule that `res' may refer to, if there is one.
 * @param res the resource bundle of the zone to be constructed
 * @param ec input-output error code
 */
OlsonTimeZone::OlsonTimeZone(const UResourceBundle* top,
                             const UResourceBundle* res,
                             UErrorCode& ec) :
    finalZone(0), finalYear(INT32_MAX) {
    if ((top == NULL || res == NULL) && U_SUCCESS(ec)) {
        ec = U_ILLEGAL_ARGUMENT_ERROR;
    }
    if (U_SUCCESS(ec)) {
        // TODO remove nonconst casts below when ures_* API is fixed
        setID(ures_getKey((UResourceBundle*) res)); // cast away const

        // Size 3 is a purely historical zone (no final rules);
        // size 5 is a hybrid zone, with historical and final elements.
        int32_t size = ures_getSize((UResourceBundle*) res); // cast away const
        if (size != 3 && size != 5) {
            ec = U_INVALID_FORMAT_ERROR;
        }

        // Transitions list may be emptry
        int32_t i;
        UResourceBundle* r = ures_getByIndex(res, 0, NULL, &ec);
        transitionTimes = ures_getIntVector(r, &i, &ec);
        ures_close(r);
        if ((i<0 || i>0x7FFF) && U_SUCCESS(ec)) {
            ec = U_INVALID_FORMAT_ERROR;
        }
        transitionCount = (int16_t) i;
        
        // Type offsets list must be of even size, with size >= 2
        r = ures_getByIndex(res, 1, NULL, &ec);
        typeOffsets = ures_getIntVector(r, &i, &ec);
        ures_close(r);
        if ((i<2 || i>0x7FFE || ((i&1)!=0)) && U_SUCCESS(ec)) {
            ec = U_INVALID_FORMAT_ERROR;
        }
        typeCount = (int16_t) i >> 1;

        // Type data must be of the same size as the transitions list        
        r = ures_getByIndex(res, 2, NULL, &ec);
        int32_t len;
        typeData = ures_getBinary(r, &len, &ec);
        ures_close(r);
        if (len != transitionCount && U_SUCCESS(ec)) {
            ec = U_INVALID_FORMAT_ERROR;
        }

        // Process final rule and data, if any
        if (size == 5) {
            UnicodeString ruleid = ures_getUnicodeStringByIndex(res, 3, &ec);
            r = ures_getByIndex(res, 4, NULL, &ec);
            const int32_t* data = ures_getIntVector(r, &len, &ec);
            ures_close(r);
            if (U_SUCCESS(ec)) {
                if (data != 0 && len == 2) {
                    int32_t rawOffset = data[0] * U_MILLIS_PER_SECOND;
                    // Subtract one from the actual final year; we
                    // actually store final year - 1, and compare
                    // using > rather than >=.  This allows us to use
                    // INT32_MAX as an exclusive upper limit for all
                    // years, including INT32_MAX.
                    U_ASSERT(data[1] > INT32_MIN);
                    finalYear = data[1] - 1;
                    char key[64];
                    key[0] = '_';
                    ruleid.extract(0, sizeof(key)-2, key+1, sizeof(key)-1, "");
                    r = ures_getByKey(top, key, NULL, &ec);
                    if (U_SUCCESS(ec)) {
                        // 3, 1, -1, 7200, 0, 9, -31, -1, 7200, 0, 3600
                        data = ures_getIntVector(r, &len, &ec);
                        if (U_SUCCESS(ec) && len == 11) {
                            finalZone = new SimpleTimeZone(rawOffset, "",
                                (int8_t)data[0], (int8_t)data[1], (int8_t)data[2],
                                data[3] * U_MILLIS_PER_SECOND,
                                (SimpleTimeZone::TimeMode) data[4],
                                (int8_t)data[5], (int8_t)data[6], (int8_t)data[7],
                                data[8] * U_MILLIS_PER_SECOND,
                                (SimpleTimeZone::TimeMode) data[9],
                                data[10] * U_MILLIS_PER_SECOND, ec);
                        } else {
                            ec = U_INVALID_FORMAT_ERROR;
                        }
                    }
                    ures_close(r);
                } else {
                    ec = U_INVALID_FORMAT_ERROR;
                }
            }
        }
    }

    if (U_FAILURE(ec)) {
        constructEmpty();
    }
}

/**
 * Copy constructor
 */
OlsonTimeZone::OlsonTimeZone(const OlsonTimeZone& other) :
    TimeZone(other), finalZone(0) {
    *this = other;
}

/**
 * Assignment operator
 */
OlsonTimeZone& OlsonTimeZone::operator=(const OlsonTimeZone& other) {
    transitionCount = other.transitionCount;
    typeCount = other.typeCount;
    transitionTimes = other.transitionTimes;
    typeOffsets = other.typeOffsets;
    typeData = other.typeData;
    finalYear = other.finalYear;
    delete finalZone;
    finalZone = (other.finalZone != 0) ?
        (SimpleTimeZone*) other.finalZone->clone() : 0;
    return *this;
}

/**
 * Destructor
 */
OlsonTimeZone::~OlsonTimeZone() {
    delete finalZone;
}

/**
 * Returns true if the two TimeZone objects are equal.
 */
UBool OlsonTimeZone::operator==(const TimeZone& other) const {
    const OlsonTimeZone* z = (const OlsonTimeZone*) &other;

    return TimeZone::operator==(other) &&
        // [sic] pointer comparison: typeData points into
        // memory-mapped or DLL space, so if two zones have the same
        // pointer, they are equal.
        (typeData == z->typeData ||
         // If the pointers are not equal, the zones may still
         // be equal if their rules and transitions are equal
         (finalYear == z->finalYear &&
          ((finalZone == 0 && z->finalZone == 0) ||
           (finalZone != 0 && z->finalZone != 0 &&
            *finalZone == *z->finalZone)) &&
          transitionCount == z->transitionCount &&
          typeCount == z->typeCount &&
          uprv_memcmp(transitionTimes, z->transitionTimes,
                      sizeof(transitionTimes[0]) * transitionCount) == 0 &&
          uprv_memcmp(typeOffsets, z->typeOffsets,
                      (sizeof(typeOffsets[0]) * typeCount) << 1) == 0 &&
          uprv_memcmp(typeData, z->typeData,
                      (sizeof(typeData[0]) * typeCount)) == 0
          ));
}

/**
 * TimeZone API.
 */
TimeZone* OlsonTimeZone::clone() const {
    return new OlsonTimeZone(*this);
}

/**
 * TimeZone API.
 */
int32_t OlsonTimeZone::getOffset(uint8_t era, int32_t year, int32_t month,
                                 int32_t dom, uint8_t dow,
                                 int32_t millis, UErrorCode& ec) const {
    if (month < UCAL_JANUARY || month > UCAL_DECEMBER) {
        if (U_SUCCESS(ec)) {
            ec = U_ILLEGAL_ARGUMENT_ERROR;
        }
        return 0;
    } else {
        return getOffset(era, year, month, dom, dow, millis,
                         MONTH_LENGTH[month + isLeapYear(year)?12:0],
                         ec);
    }
}

/**
 * TimeZone API.
 */
int32_t OlsonTimeZone::getOffset(uint8_t era, int32_t year, int32_t month,
                                 int32_t dom, uint8_t dow,
                                 int32_t millis, int32_t monthLength,
                                 UErrorCode& ec) const {
    if (U_FAILURE(ec)) {
        return 0;
    }

    if ((era != GregorianCalendar::AD && era != GregorianCalendar::BC)
        || month < UCAL_JANUARY
        || month > UCAL_DECEMBER
        || dom < 1
        || dom > monthLength
        || dow < UCAL_SUNDAY
        || dow > UCAL_SATURDAY
        || millis < 0
        || millis >= U_MILLIS_PER_DAY
        || monthLength < 28
        || monthLength > 31) {
        ec = U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    if (era == GregorianCalendar::BC) {
        year = -year;
    }

    if (year > finalYear) { // [sic] >, not >=; see above
        U_ASSERT(finalZone != 0);
        return finalZone->getOffset(era, year, month, dom, dow,
                                    millis, monthLength, ec);
    }

    // Compute local epoch seconds from input fields
    double time = fieldsToDay(year, month, dom) * SECONDS_PER_DAY +
        uprv_floor(millis / (double) U_MILLIS_PER_SECOND);

    return zoneOffset(findTransition(time, TRUE)) * U_MILLIS_PER_SECOND;
}

/**
 * TimeZone API.
 */
void OlsonTimeZone::setRawOffset(int32_t /*offsetMillis*/) {
    // We don't support this operation, since OlsonTimeZones are
    // immutable (except for the ID, which is in the base class).

    // Nothing to do!
}

/**
 * TimeZone API.
 */
int32_t OlsonTimeZone::getRawOffset() const {
    UErrorCode ec = U_ZERO_ERROR;
    int32_t raw, dst;
    getOffset((double) uprv_getUTCtime() * U_MILLIS_PER_SECOND,
              FALSE, raw, dst, ec);
    return raw;
}

/**
 * TimeZone API.
 */
void OlsonTimeZone::getOffset(UDate date, UBool local, int32_t& rawoff,
                              int32_t& dstoff, UErrorCode& ec) const {
    if (U_FAILURE(ec)) {
        return;
    }

    int32_t year, month, dom, dow;
    double secs = uprv_floor(date / U_MILLIS_PER_SECOND);
    double days = uprv_floor(date / U_MILLIS_PER_DAY);

    dayToFields(days, year, month, dom, dow);

    if (year > finalYear) { // [sic] >, not >=; see above
        U_ASSERT(finalZone != 0);
        int32_t millis = (int32_t) (date - days * U_MILLIS_PER_DAY);
        rawoff = finalZone->getRawOffset();

        if (!local) {
            // Adjust from GMT to local
            date += rawoff;
            double days2 = uprv_floor(date / U_MILLIS_PER_DAY);
            millis = (int32_t) (date - days2 * U_MILLIS_PER_DAY);
            if (days2 != days) {
                dayToFields(days2, year, month, dom, dow);
            }
        }

        dstoff = finalZone->getOffset(
            GregorianCalendar::AD, year, month,
            dom, (uint8_t) dow, millis, ec) - rawoff;
        return;
    }

    int16_t i = findTransition(secs, local);
    rawoff = rawOffset(i) * U_MILLIS_PER_SECOND;
    dstoff = dstOffset(i) * U_MILLIS_PER_SECOND;
}

/**
 * Find the smallest i (in 0..transitionCount-1) such that time >=
 * transition(i), where transition(i) is either the GMT or the local
 * transition time, as specified by `local'.
 * @param time epoch seconds, either GMT or local wall
 * @param local if TRUE, `time' is in local wall units, otherwise it
 * is GMT
 * @return an index i, where 0 <= i < transitionCount, and
 * transition(i) <= time < transition(i+1), or i == 0 if
 * transitionCount == 0 or time < transition(0).
 */
int16_t OlsonTimeZone::findTransition(double time, UBool local) const {
    int16_t i = 0;
    
    if (transitionCount != 0) {
        // Linear search from the end is the fastest approach, since
        // most lookups will happen at/near the end.
        for (i = transitionCount - 1; i > 0; --i) {
            int32_t transition = transitionTimes[i];
            if (local) {
                transition += zoneOffset(typeData[i]);
            }
            if (time >= transition) {
                break;
            }
        }

        U_ASSERT(i>=0 && i<transitionCount);

        // Check invariants for GMT times; if these pass for GMT times
        // the local logic should be working too.
        U_ASSERT(local || time < transitionTimes[0] || time >= transitionTimes[i]);
        U_ASSERT(local || i == transitionCount-1 || time < transitionTimes[i+1]);

        i = typeData[i];
    }

    U_ASSERT(i>=0 && i<typeCount);
    
    return i;
}

/**
 * TimeZone API.
 */
UBool OlsonTimeZone::useDaylightTime() const {
    // If DST was observed in 1942 (for example) but has never been
    // observed from 1943 to the present, most clients will expect
    // this method to return FALSE.  This method determines whether
    // DST is in use in the current year (at any point in the year)
    // and returns TRUE if so.

    int32_t days = floorDivide(uprv_getUTCtime(), SECONDS_PER_DAY); // epoch days

    int32_t year, month, dom, dow;
    
    dayToFields(days, year, month, dom, dow);

    if (year > finalYear) { // [sic] >, not >=; see above
        U_ASSERT(finalZone != 0 && finalZone->useDaylightTime());
        return TRUE;
    }

    // Find start of this year, and start of next year
    int32_t start = (int32_t) fieldsToDay(year, 0, 1) * SECONDS_PER_DAY;    
    int32_t limit = (int32_t) fieldsToDay(year+1, 0, 1) * SECONDS_PER_DAY;    

    // Return TRUE if DST is observed at any time during the current
    // year.
    for (int16_t i=0; i<transitionCount; ++i) {
        if (transitionTimes[i] >= limit) {
            break;
        }
        if (transitionTimes[i] >= start &&
            dstOffset(typeData[i]) != 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * TimeZone API.
 */
UBool OlsonTimeZone::inDaylightTime(UDate date, UErrorCode& ec) const {
    int32_t raw, dst;
    getOffset(date, FALSE, raw, dst, ec);
    return dst != 0;
}

U_NAMESPACE_END

#endif // !UCONFIG_NO_FORMATTING

//eof
