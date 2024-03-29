// © 2017 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html

// extradata.cpp
// created: 2017jun04 Markus W. Scherer
// (pulled out of n2builder.cpp)

#include "unicode/utypes.h"

#if !UCONFIG_NO_NORMALIZATION

#include <stdio.h>
#include <stdlib.h>
#include "unicode/errorcode.h"
#include "unicode/unistr.h"
#include "unicode/utf16.h"
#include "extradata.h"
#include "normalizer2impl.h"
#include "norms.h"
#include "toolutil.h"
#include "utrie2.h"
#include "uvectr32.h"

U_NAMESPACE_BEGIN

ExtraData::ExtraData(Norms &n, UBool fast) :
        Norms::Enumerator(n),
        yesYesCompositions(1000, (UChar32)0xffff, 2),  // 0=inert, 1=Jamo L, 2=start of compositions
        yesNoMappingsAndCompositions(1000, (UChar32)0, 1),  // 0=Hangul, 1=start of normal data
        optimizeFast(fast) {}

int32_t ExtraData::writeMapping(UChar32 c, const Norm &norm, UnicodeString &dataString) {
    UnicodeString &m=*norm.mapping;
    int32_t length=m.length();
    if(length>Normalizer2Impl::MAPPING_LENGTH_MASK) {
        fprintf(stderr,
                "gennorm2 error: "
                "mapping for U+%04lX longer than maximum of %d\n",
                (long)c, Normalizer2Impl::MAPPING_LENGTH_MASK);
        exit(U_INVALID_FORMAT_ERROR);
    }
    // Write the mapping & raw mapping extraData.
    int32_t firstUnit=length|(norm.trailCC<<8);
    int32_t preMappingLength=0;
    if(norm.rawMapping!=NULL) {
        UnicodeString &rm=*norm.rawMapping;
        int32_t rmLength=rm.length();
        if(rmLength>Normalizer2Impl::MAPPING_LENGTH_MASK) {
            fprintf(stderr,
                    "gennorm2 error: "
                    "raw mapping for U+%04lX longer than maximum of %d\n",
                    (long)c, Normalizer2Impl::MAPPING_LENGTH_MASK);
            exit(U_INVALID_FORMAT_ERROR);
        }
        UChar rm0=rm.charAt(0);
        if( rmLength==length-1 &&
            // 99: overlong substring lengths get pinned to remainder lengths anyway
            0==rm.compare(1, 99, m, 2, 99) &&
            rm0>Normalizer2Impl::MAPPING_LENGTH_MASK
        ) {
            // Compression:
            // rawMapping=rm0+mapping.substring(2) -> store only rm0
            //
            // The raw mapping is the same as the final mapping after replacing
            // the final mapping's first two code units with the raw mapping's first one.
            // In this case, we store only that first unit, rm0.
            // This helps with a few hundred mappings.
            dataString.append(rm0);
            preMappingLength=1;
        } else {
            // Store the raw mapping with its length.
            dataString.append(rm);
            dataString.append((UChar)rmLength);
            preMappingLength=rmLength+1;
        }
        firstUnit|=Normalizer2Impl::MAPPING_HAS_RAW_MAPPING;
    }
    int32_t cccLccc=norm.cc|(norm.leadCC<<8);
    if(cccLccc!=0) {
        dataString.append((UChar)cccLccc);
        ++preMappingLength;
        firstUnit|=Normalizer2Impl::MAPPING_HAS_CCC_LCCC_WORD;
    }
    if(norm.hasNoCompBoundaryAfter) {
        firstUnit|=Normalizer2Impl::MAPPING_NO_COMP_BOUNDARY_AFTER;
    }
    dataString.append((UChar)firstUnit);
    dataString.append(m);
    return preMappingLength;
}

int32_t ExtraData::writeNoNoMapping(UChar32 c, const Norm &norm,
                                    UnicodeString &dataString,
                                    Hashtable &previousMappings) {
    UnicodeString newMapping;
    int32_t offset=writeMapping(c, norm, newMapping);
    int32_t previousOffset=previousMappings.geti(newMapping);
    if(previousOffset!=0) {
        // Duplicate, point to the identical mapping that has already been stored.
        offset=previousOffset-1;
    } else {
        // Append this new mapping and
        // enter it into the hashtable, avoiding value 0 which is "not found".
        offset=dataString.length()+offset;
        dataString.append(newMapping);
        IcuToolErrorCode errorCode("gennorm2/writeExtraData()/Hashtable.puti()");
        previousMappings.puti(newMapping, offset+1, errorCode);
    }
    return offset;
}

void ExtraData::writeCompositions(UChar32 c, const Norm &norm, UnicodeString &dataString) {
    if(norm.cc!=0) {
        fprintf(stderr,
                "gennorm2 error: "
                "U+%04lX combines-forward and has ccc!=0, not possible in Unicode normalization\n",
                (long)c);
        exit(U_INVALID_FORMAT_ERROR);
    }
    int32_t length;
    const CompositionPair *pairs=norm.getCompositionPairs(length);
    for(int32_t i=0; i<length; ++i) {
        const CompositionPair &pair=pairs[i];
        // 22 bits for the composite character and whether it combines forward.
        UChar32 compositeAndFwd=pair.composite<<1;
        if(norms.getNormRef(pair.composite).compositions!=NULL) {
            compositeAndFwd|=1;  // The composite character also combines-forward.
        }
        // Encode most pairs in two units and some in three.
        int32_t firstUnit, secondUnit, thirdUnit;
        if(pair.trail<Normalizer2Impl::COMP_1_TRAIL_LIMIT) {
            if(compositeAndFwd<=0xffff) {
                firstUnit=pair.trail<<1;
                secondUnit=compositeAndFwd;
                thirdUnit=-1;
            } else {
                firstUnit=(pair.trail<<1)|Normalizer2Impl::COMP_1_TRIPLE;
                secondUnit=compositeAndFwd>>16;
                thirdUnit=compositeAndFwd;
            }
        } else {
            firstUnit=(Normalizer2Impl::COMP_1_TRAIL_LIMIT+
                       (pair.trail>>Normalizer2Impl::COMP_1_TRAIL_SHIFT))|
                      Normalizer2Impl::COMP_1_TRIPLE;
            secondUnit=(pair.trail<<Normalizer2Impl::COMP_2_TRAIL_SHIFT)|
                       (compositeAndFwd>>16);
            thirdUnit=compositeAndFwd;
        }
        // Set the high bit of the first unit if this is the last composition pair.
        if(i==(length-1)) {
            firstUnit|=Normalizer2Impl::COMP_1_LAST_TUPLE;
        }
        dataString.append((UChar)firstUnit).append((UChar)secondUnit);
        if(thirdUnit>=0) {
            dataString.append((UChar)thirdUnit);
        }
    }
}

void ExtraData::rangeHandler(UChar32 start, UChar32 end, Norm &norm) {
    if(start!=end) {
        fprintf(stderr,
                "gennorm2 error: unexpected shared data for "
                "multiple code points U+%04lX..U+%04lX\n",
                (long)start, (long)end);
        exit(U_INTERNAL_PROGRAM_ERROR);
    }
    if(norm.error!=nullptr) {
        fprintf(stderr, "gennorm2 error: U+%04lX %s\n", (long)start, norm.error);
        exit(U_INVALID_FORMAT_ERROR);
    }
    writeExtraData(start, norm);
}

void ExtraData::writeExtraData(UChar32 c, Norm &norm) {
    switch(norm.type) {
    case Norm::INERT:
        break;  // no extra data
    case Norm::YES_YES_COMBINES_FWD:
        norm.offset=yesYesCompositions.length();
        writeCompositions(c, norm, yesYesCompositions);
        break;
    case Norm::YES_NO_COMBINES_FWD:
        norm.offset=yesNoMappingsAndCompositions.length()+
                writeMapping(c, norm, yesNoMappingsAndCompositions);
        writeCompositions(c, norm, yesNoMappingsAndCompositions);
        break;
    case Norm::YES_NO_MAPPING_ONLY:
        norm.offset=yesNoMappingsOnly.length()+
                writeMapping(c, norm, yesNoMappingsOnly);
        break;
    case Norm::NO_NO:
        if(norm.cc==0 && !optimizeFast) {
            // Try a compact, algorithmic encoding.
            // Only for ccc=0, because we can't store additional information
            // and we do not recursively follow an algorithmic encoding for access to the ccc.
            //
            // Also, if hasNoCompBoundaryAfter is set, we can only use the algorithmic encoding
            // if the mappingCP decomposes further, to ensure that there is a place to store it.
            // We want to see that the final mapping does not have exactly 1 code point,
            // or else we would have to recursively ensure that the final mapping is stored
            // in normal extraData.
            if(norm.mappingCP>=0 &&
                    (!norm.hasNoCompBoundaryAfter || 1!=norm.mapping->countChar32())) {
                int32_t delta=norm.mappingCP-c;
                if(-Normalizer2Impl::MAX_DELTA<=delta && delta<=Normalizer2Impl::MAX_DELTA) {
                    norm.type=Norm::NO_NO_DELTA;
                    norm.offset=delta;
                    break;
                }
            }
        }
        // TODO: minMappingNotCompYes, minMappingNoCompBoundaryBefore
        norm.offset=writeNoNoMapping(c, norm, noNoMappings, previousNoNoMappings);
        break;
    case Norm::MAYBE_YES_COMBINES_FWD:
        norm.offset=maybeYesCompositions.length();
        writeCompositions(c, norm, maybeYesCompositions);
        break;
    case Norm::MAYBE_YES_SIMPLE:
        break;  // no extra data
    case Norm::YES_YES_WITH_CC:
        break;  // no extra data
    default:  // Should not occur.
        exit(U_INTERNAL_PROGRAM_ERROR);
    }
}

U_NAMESPACE_END

#endif // #if !UCONFIG_NO_NORMALIZATION
