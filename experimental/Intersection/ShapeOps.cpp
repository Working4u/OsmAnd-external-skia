/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "Simplify.h"

namespace Op {

#include "Simplify.cpp"

// FIXME: this and find chase should be merge together, along with
// other code that walks winding in angles
// OPTIMIZATION: Probably, the walked winding should be rolled into the angle structure
// so it isn't duplicated by walkers like this one
static Segment* findChaseOp(SkTDArray<Span*>& chase, int& tIndex, int& endIndex) {
    while (chase.count()) {
        Span* span;
        chase.pop(&span);
        const Span& backPtr = span->fOther->span(span->fOtherIndex);
        Segment* segment = backPtr.fOther;
        tIndex = backPtr.fOtherIndex;
        SkTDArray<Angle> angles;
        int done = 0;
        if (segment->activeAngle(tIndex, done, angles)) {
            Angle* last = angles.end() - 1;
            tIndex = last->start();
            endIndex = last->end();
   #if TRY_ROTATE
            *chase.insert(0) = span;
   #else
            *chase.append() = span;
   #endif
            return last->segment();
        }
        if (done == angles.count()) {
            continue;
        }
        SkTDArray<Angle*> sorted;
        bool sortable = Segment::SortAngles(angles, sorted);
#if DEBUG_SORT
        sorted[0]->segment()->debugShowSort(__FUNCTION__, sorted, 0, 0, 0);
#endif
        if (!sortable) {
            continue;
        }
        // find first angle, initialize winding to computed fWindSum
        int firstIndex = -1;
        const Angle* angle;
        int winding;
        do {
            angle = sorted[++firstIndex];
            segment = angle->segment();
            winding = segment->windSum(angle);
        } while (winding == SK_MinS32);
        int spanWinding = segment->spanSign(angle->start(), angle->end());
    #if DEBUG_WINDING
        SkDebugf("%s winding=%d spanWinding=%d\n",
                __FUNCTION__, winding, spanWinding);
    #endif
        // turn span winding into contour winding
        if (spanWinding * winding < 0) {
            winding += spanWinding;
        }
        // we care about first sign and whether wind sum indicates this
        // edge is inside or outside. Maybe need to pass span winding
        // or first winding or something into this function?
        // advance to first undone angle, then return it and winding
        // (to set whether edges are active or not)
        int nextIndex = firstIndex + 1;
        int angleCount = sorted.count();
        int lastIndex = firstIndex != 0 ? firstIndex : angleCount;
        angle = sorted[firstIndex];
        segment = angle->segment();
        int oWinding = segment->oppSum(angle);
    #if DEBUG_SORT
        segment->debugShowSort(__FUNCTION__, sorted, firstIndex, winding, oWinding);
    #endif
        winding -= segment->spanSign(angle);
        bool firstOperand = segment->operand();
        do {
            SkASSERT(nextIndex != firstIndex);
            if (nextIndex == angleCount) {
                nextIndex = 0;
            }
            angle = sorted[nextIndex];
            segment = angle->segment();
            int deltaSum = segment->spanSign(angle);
            bool angleIsOp = segment->operand() ^ firstOperand;
            int maxWinding;
            if (angleIsOp) {
                maxWinding = oWinding;
                oWinding -= deltaSum;
            } else {
                maxWinding = winding;
                winding -= deltaSum;
            }
    #if DEBUG_SORT
            SkDebugf("%s id=%d maxWinding=%d winding=%d oWinding=%d sign=%d\n", __FUNCTION__,
                    segment->debugID(), maxWinding, winding, oWinding, angle->sign());
    #endif
            tIndex = angle->start();
            endIndex = angle->end();
            int lesser = SkMin32(tIndex, endIndex);
            const Span& nextSpan = segment->span(lesser);
            if (!nextSpan.fDone) {
                if (angleIsOp) {
                    SkTSwap(winding, oWinding);
                }
                if (useInnerWinding(maxWinding, winding)) {
                    maxWinding = winding;
                }
                segment->markWinding(lesser, maxWinding, oWinding);
                break;
            }
        } while (++nextIndex != lastIndex);
   #if TRY_ROTATE
        *chase.insert(0) = span;
   #else
        *chase.append() = span;
   #endif
        return segment;
    }
    return NULL;
}

static bool windingIsActive(int winding, int oppWinding, int spanWinding,
        bool windingIsOp, ShapeOp op) {
    bool active = windingIsActive(winding, spanWinding);
    if (!active) {
        return false;
    }
    bool opActive = oppWinding != 0;
    return gOpLookup[op][opActive][windingIsOp];
}

static bool bridgeOp(SkTDArray<Contour*>& contourList, const ShapeOp op,
        const int aXorMask, const int bXorMask, PathWrapper& simple) {
    bool firstContour = true;
    bool unsortable = false;
    bool closable = true;
    SkPoint topLeft = {SK_ScalarMin, SK_ScalarMin};
    do {
        int index, endIndex;
        Segment* current = findSortableTop(contourList, index, endIndex, topLeft);
        if (!current) {
            break;
        }
        int contourWinding, oppContourWinding;
        if (firstContour) {
            contourWinding = oppContourWinding = 0;
            firstContour = false;
        } else {
            int sumWinding = current->windSum(SkMin32(index, endIndex));
            // FIXME: don't I have to adjust windSum to get contourWinding?
            if (sumWinding == SK_MinS32) {
                sumWinding = current->computeSum(index, endIndex);
            }
            if (sumWinding == SK_MinS32) {
                contourWinding = innerContourCheck(contourList, current,
                        index, endIndex, false);
                oppContourWinding = innerContourCheck(contourList, current,
                        index, endIndex, true);
            } else {
                contourWinding = sumWinding;
                oppContourWinding = 0;
                SkASSERT(0);
                // FIXME: need to get oppContourWinding by building sort wheel and
                // retrieving sumWinding of uphill opposite span, calling inner contour check
                // if need be
                int spanWinding = current->spanSign(index, endIndex);
                bool inner = useInnerWinding(sumWinding - spanWinding, sumWinding);
                if (inner) {
                    contourWinding -= spanWinding;
                }
#if DEBUG_WINDING
                SkDebugf("%s sumWinding=%d spanWinding=%d sign=%d inner=%d result=%d\n", __FUNCTION__,
                        sumWinding, spanWinding, SkSign32(index - endIndex),
                        inner, contourWinding);
#endif
            }
#if DEBUG_WINDING
         //   SkASSERT(current->debugVerifyWinding(index, endIndex, contourWinding));
            SkDebugf("%s contourWinding=%d\n", __FUNCTION__, contourWinding);
#endif
        }
        int winding = contourWinding;
        int oppWinding = oppContourWinding;
        int spanWinding = current->spanSign(index, endIndex);
        SkTDArray<Span*> chaseArray;
        do {
            bool active = windingIsActive(winding, oppWinding, spanWinding,
                    current->operand(), op);
        #if DEBUG_WINDING
            SkDebugf("%s active=%s winding=%d oppWinding=%d spanWinding=%d\n",
                    __FUNCTION__, active ? "true" : "false",
                    winding, oppWinding, spanWinding);
        #endif
            do {
        #if DEBUG_ACTIVE_SPANS
                if (!unsortable && current->done()) {
                    debugShowActiveSpans(contourList);
                }
        #endif
                SkASSERT(unsortable || !current->done());
                int nextStart = index;
                int nextEnd = endIndex;
                Segment* next = current->findNextOp(chaseArray, active,
                        nextStart, nextEnd, winding, oppWinding, spanWinding,
                        unsortable, op, aXorMask, bXorMask);
                if (!next) {
                    SkASSERT(!unsortable);
                    if (active && !unsortable && simple.hasMove()
                            && current->verb() != SkPath::kLine_Verb
                            && !simple.isClosed()) {
                        current->addCurveTo(index, endIndex, simple, true);
                        SkASSERT(simple.isClosed());
                    }
                    break;
                }
                current->addCurveTo(index, endIndex, simple, active);
                current = next;
                index = nextStart;
                endIndex = nextEnd;
            } while (!simple.isClosed()
                    && ((active && !unsortable) || !current->done()));
            if (active) {
                if (!simple.isClosed()) {
                    SkASSERT(unsortable);
                    int min = SkMin32(index, endIndex);
                    if (!current->done(min)) {
                        current->addCurveTo(index, endIndex, simple, true);
                        current->markDone(SkMin32(index, endIndex), winding ? winding : spanWinding);
                    }
                    closable = false;
                }
                simple.close();
            }
            current = findChaseOp(chaseArray, index, endIndex);
        #if DEBUG_ACTIVE_SPANS
            debugShowActiveSpans(contourList);
        #endif
            if (!current) {
                break;
            }
            winding = updateWindings(current, index, endIndex, spanWinding, &oppWinding);
        } while (true);
    } while (true);
    return closable;
}

} // end of Op namespace


void operate(const SkPath& one, const SkPath& two, ShapeOp op, SkPath& result) {
    result.reset();
    result.setFillType(SkPath::kEvenOdd_FillType);
    // turn path into list of segments
    SkTArray<Op::Contour> contours;
    // FIXME: add self-intersecting cubics' T values to segment
    Op::EdgeBuilder builder(one, contours);
    const int aXorMask = builder.xorMask();
    builder.addOperand(two);
    const int bXorMask = builder.xorMask();
    builder.finish();
    SkTDArray<Op::Contour*> contourList;
    makeContourList(contours, contourList);
    Op::Contour** currentPtr = contourList.begin();
    if (!currentPtr) {
        return;
    }
    Op::Contour** listEnd = contourList.end();
    // find all intersections between segments
    do {
        Op::Contour** nextPtr = currentPtr;
        Op::Contour* current = *currentPtr++;
        Op::Contour* next;
        do {
            next = *nextPtr++;
        } while (addIntersectTs(current, next) && nextPtr != listEnd);
    } while (currentPtr != listEnd);
    // eat through coincident edges
    coincidenceCheck(contourList);
    fixOtherTIndex(contourList);
    sortSegments(contourList);
#if DEBUG_ACTIVE_SPANS
    debugShowActiveSpans(contourList);
#endif
    // construct closed contours
    Op::PathWrapper wrapper(result);
    bridgeOp(contourList, op, aXorMask, bXorMask, wrapper);
}
