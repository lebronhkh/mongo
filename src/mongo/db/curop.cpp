/**
*    Copyright (C) 2009 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/curop.h"

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/json.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;

/**
 * This type decorates a Client object with a stack of active CurOp objects.
 *
 * It encapsulates the nesting logic for curops attached to a Client, along with
 * the notion that there is always a root CurOp attached to a Client.
 *
 * The stack itself is represented in the _parent pointers of the CurOp class.
 */
class CurOp::CurOpStack {
    MONGO_DISALLOW_COPYING(CurOpStack);

public:
    CurOpStack() : _base(nullptr, this) {}

    /**
     * Returns the top of the CurOp stack.
     */
    CurOp* top() const {
        return _top;
    }

    /**
     * Adds "curOp" to the top of the CurOp stack for a client. Called by CurOp's constructor.
     */
    void push(OperationContext* opCtx, CurOp* curOp) {
        invariant(opCtx);
        if (_opCtx) {
            invariant(_opCtx == opCtx);
        } else {
            _opCtx = opCtx;
        }
        stdx::lock_guard<Client> lk(*_opCtx->getClient());
        push_nolock(curOp);
    }

    void push_nolock(CurOp* curOp) {
        invariant(!curOp->_parent);
        curOp->_parent = _top;
        _top = curOp;
    }

    /**
     * Pops the top off the CurOp stack for a Client. Called by CurOp's destructor.
     */
    CurOp* pop() {
        // It is not necessary to lock when popping the final item off of the curop stack. This
        // is because the item at the base of the stack is owned by the stack itself, and is not
        // popped until the stack is being destroyed.  By the time the stack is being destroyed,
        // no other threads can be observing the Client that owns the stack, because it has been
        // removed from its ServiceContext's set of owned clients.  Further, because the last
        // item is popped in the destructor of the stack, and that destructor runs during
        // destruction of the owning client, it is not safe to access other member variables of
        // the client during the final pop.
        const bool shouldLock = _top->_parent;
        if (shouldLock) {
            invariant(_opCtx);
            _opCtx->getClient()->lock();
        }
        invariant(_top);
        CurOp* retval = _top;
        _top = _top->_parent;
        if (shouldLock) {
            _opCtx->getClient()->unlock();
        }
        return retval;
    }

private:
    OperationContext* _opCtx = nullptr;

    // Top of the stack of CurOps for a Client.
    CurOp* _top = nullptr;

    // The bottom-most CurOp for a client.
    const CurOp _base;
};

const OperationContext::Decoration<CurOp::CurOpStack> CurOp::_curopStack =
    OperationContext::declareDecoration<CurOp::CurOpStack>();

// Enabling the maxTimeAlwaysTimeOut fail point will cause any query or command run with a
// valid non-zero max time to fail immediately.  Any getmore operation on a cursor already
// created with a valid non-zero max time will also fail immediately.
//
// This fail point cannot be used with the maxTimeNeverTimeOut fail point.
MONGO_FP_DECLARE(maxTimeAlwaysTimeOut);

// Enabling the maxTimeNeverTimeOut fail point will cause the server to never time out any
// query, command, or getmore operation, regardless of whether a max time is set.
//
// This fail point cannot be used with the maxTimeAlwaysTimeOut fail point.
MONGO_FP_DECLARE(maxTimeNeverTimeOut);


BSONObj CachedBSONObjBase::_tooBig = fromjson("{\"$msg\":\"query not recording (too large)\"}");


CurOp* CurOp::get(const OperationContext* opCtx) {
    return get(*opCtx);
}

CurOp* CurOp::get(const OperationContext& opCtx) {
    return _curopStack(opCtx).top();
}

CurOp::CurOp(OperationContext* opCtx) : CurOp(opCtx, &_curopStack(opCtx)) {}

CurOp::CurOp(OperationContext* opCtx, CurOpStack* stack) : _stack(stack) {
    if (opCtx) {
        _stack->push(opCtx, this);
    } else {
        _stack->push_nolock(this);
    }
    _start = 0;
    _isCommand = false;
    _dbprofile = 0;
    _end = 0;
    _maxTimeMicros = 0;
    _maxTimeTracker.reset();
    _message = "";
    _progressMeter.finished();
    _numYields = 0;
    _expectedLatencyMs = 0;
    _op = 0;
    _command = NULL;
}

void CurOp::setOp_inlock(int op) {
    _op = op;
}

ProgressMeter& CurOp::setMessage_inlock(const char* msg,
                                        std::string name,
                                        unsigned long long progressMeterTotal,
                                        int secondsBetween) {
    if (progressMeterTotal) {
        if (_progressMeter.isActive()) {
            error() << "old _message: " << _message << " new message:" << msg;
            verify(!_progressMeter.isActive());
        }
        _progressMeter.reset(progressMeterTotal, secondsBetween);
        _progressMeter.setName(name);
    } else {
        _progressMeter.finished();
    }
    _message = msg;
    return _progressMeter;
}

CurOp::~CurOp() {
    invariant(this == _stack->pop());
}

void CurOp::setNS_inlock(StringData ns) {
    _ns = ns.toString();
}

void CurOp::ensureStarted() {
    if (_start == 0) {
        _start = curTimeMicros64();

        // If ensureStarted() is invoked after setMaxTimeMicros(), then time limit tracking will
        // start here.  This is because time limit tracking can only commence after the
        // operation is assigned a start time.
        if (_maxTimeMicros > 0) {
            _maxTimeTracker.setTimeLimit(_start, _maxTimeMicros);
        }
    }
}

void CurOp::enter_inlock(const char* ns, int dbProfileLevel) {
    ensureStarted();
    _ns = ns;
    raiseDbProfileLevel(dbProfileLevel);
}

void CurOp::raiseDbProfileLevel(int dbProfileLevel) {
    _dbprofile = std::max(dbProfileLevel, _dbprofile);
}

void CurOp::reportState(BSONObjBuilder* builder) {
    if (_start) {
        builder->append("secs_running", elapsedSeconds());
        builder->append("microsecs_running", static_cast<long long int>(elapsedMicros()));
    }

    builder->append("op", opToString(_op));

    // Fill out "ns" from our namespace member (and if it's not available, fall back to the
    // OpDebug namespace member). We prefer our ns when set because it changes to match each
    // accessed namespace, while _debug.ns is set once at the start of the operation. However,
    // sometimes _ns is not yet set.
    builder->append("ns", !_ns.empty() ? _ns : _debug.ns);

    if (_op == dbInsert) {
        _query.append(*builder, "insert");
    } else {
        _query.append(*builder, "query");
    }

    if (!debug().planSummary.empty()) {
        builder->append("planSummary", debug().planSummary.toString());
    }

    if (!_message.empty()) {
        if (_progressMeter.isActive()) {
            StringBuilder buf;
            buf << _message << " " << _progressMeter.toString();
            builder->append("msg", buf.str());
            BSONObjBuilder sub(builder->subobjStart("progress"));
            sub.appendNumber("done", (long long)_progressMeter.done());
            sub.appendNumber("total", (long long)_progressMeter.total());
            sub.done();
        } else {
            builder->append("msg", _message);
        }
    }

    builder->append("numYields", _numYields);
}

void CurOp::setMaxTimeMicros(uint64_t maxTimeMicros) {
    _maxTimeMicros = maxTimeMicros;

    if (_maxTimeMicros == 0) {
        // 0 is "allow to run indefinitely".
        return;
    }

    // If the operation has a start time, then enable the tracker.
    //
    // If the operation has no start time yet, then ensureStarted() will take responsibility for
    // enabling the tracker.
    if (isStarted()) {
        _maxTimeTracker.setTimeLimit(startTime(), _maxTimeMicros);
    }
}

bool CurOp::isMaxTimeSet() const {
    return _maxTimeMicros != 0;
}

bool CurOp::maxTimeHasExpired() {
    if (MONGO_FAIL_POINT(maxTimeNeverTimeOut)) {
        return false;
    }
    if (_maxTimeMicros > 0 && MONGO_FAIL_POINT(maxTimeAlwaysTimeOut)) {
        return true;
    }
    return _maxTimeTracker.checkTimeLimit();
}

uint64_t CurOp::getRemainingMaxTimeMicros() const {
    return _maxTimeTracker.getRemainingMicros();
}

CurOp::MaxTimeTracker::MaxTimeTracker() {
    reset();
}

void CurOp::MaxTimeTracker::reset() {
    _enabled = false;
    _targetEpochMicros = 0;
    _approxTargetServerMillis = 0;
}

void CurOp::MaxTimeTracker::setTimeLimit(uint64_t startEpochMicros, uint64_t durationMicros) {
    dassert(durationMicros != 0);

    _enabled = true;

    _targetEpochMicros = startEpochMicros + durationMicros;

    uint64_t now = curTimeMicros64();
    // If our accurate time source thinks time is not up yet, calculate the next target for
    // our approximate time source.
    if (_targetEpochMicros > now) {
        _approxTargetServerMillis = Listener::getElapsedTimeMillis() +
            static_cast<int64_t>((_targetEpochMicros - now) / 1000);
    }
    // Otherwise, set our approximate time source target such that it thinks time is already
    // up.
    else {
        _approxTargetServerMillis = Listener::getElapsedTimeMillis();
    }
}

bool CurOp::MaxTimeTracker::checkTimeLimit() {
    if (!_enabled) {
        return false;
    }

    // Does our approximate time source think time is not up yet?  If so, return early.
    if (_approxTargetServerMillis > Listener::getElapsedTimeMillis()) {
        return false;
    }

    uint64_t now = curTimeMicros64();
    // Does our accurate time source think time is not up yet?  If so, readjust the target for
    // our approximate time source and return early.
    if (_targetEpochMicros > now) {
        _approxTargetServerMillis = Listener::getElapsedTimeMillis() +
            static_cast<int64_t>((_targetEpochMicros - now) / 1000);
        return false;
    }

    // Otherwise, time is up.
    return true;
}

uint64_t CurOp::MaxTimeTracker::getRemainingMicros() const {
    if (!_enabled) {
        // 0 is "allow to run indefinitely".
        return 0;
    }

    // Does our accurate time source think time is up?  If so, claim there is 1 microsecond
    // left for this operation.
    uint64_t now = curTimeMicros64();
    if (_targetEpochMicros <= now) {
        return 1;
    }

    // Otherwise, calculate remaining time.
    return _targetEpochMicros - now;
}

void OpDebug::reset() {
    extra.reset();

    op = 0;
    iscommand = false;
    ns = "";
    query = BSONObj();
    updateobj = BSONObj();

    cursorid = -1;
    ntoreturn = -1;
    ntoskip = -1;
    exhaust = false;

    nscanned = -1;
    nscannedObjects = -1;
    idhack = false;
    scanAndOrder = false;
    nMatched = -1;
    nModified = -1;
    ninserted = -1;
    ndeleted = -1;
    nmoved = -1;
    fastmod = false;
    fastmodinsert = false;
    upsert = false;
    cursorExhausted = false;
    keyUpdates = 0;  // unsigned, so -1 not possible
    writeConflicts = 0;
    planSummary = "";
    execStats.reset();

    exceptionInfo.reset();

    executionTime = 0;
    nreturned = -1;
    responseLength = -1;
}


#define OPDEBUG_TOSTRING_HELP(x) \
    if (x >= 0)                  \
    s << " " #x ":" << (x)
#define OPDEBUG_TOSTRING_HELP_BOOL(x) \
    if (x)                            \
    s << " " #x ":" << (x)
string OpDebug::report(const CurOp& curop, const SingleThreadedLockStats& lockStats) const {
    StringBuilder s;
    if (iscommand)
        s << "command ";
    else
        s << opToString(op) << ' ';
    s << ns;

    if (!query.isEmpty()) {
        if (iscommand) {
            s << " command: ";

            Command* curCommand = curop.getCommand();
            if (curCommand) {
                mutablebson::Document cmdToLog(query, mutablebson::Document::kInPlaceDisabled);
                curCommand->redactForLogging(&cmdToLog);
                s << curCommand->name << " ";
                s << cmdToLog.toString();
            } else {  // Should not happen but we need to handle curCommand == NULL gracefully
                s << query.toString();
            }
        } else {
            s << " query: ";
            s << query.toString();
        }
    }

    if (!planSummary.empty()) {
        s << " planSummary: " << planSummary.toString();
    }

    if (!updateobj.isEmpty()) {
        s << " update: ";
        updateobj.toString(s);
    }

    OPDEBUG_TOSTRING_HELP(cursorid);
    OPDEBUG_TOSTRING_HELP(ntoreturn);
    OPDEBUG_TOSTRING_HELP(ntoskip);
    OPDEBUG_TOSTRING_HELP_BOOL(exhaust);

    OPDEBUG_TOSTRING_HELP(nscanned);
    OPDEBUG_TOSTRING_HELP(nscannedObjects);
    OPDEBUG_TOSTRING_HELP_BOOL(idhack);
    OPDEBUG_TOSTRING_HELP_BOOL(scanAndOrder);
    OPDEBUG_TOSTRING_HELP(nmoved);
    OPDEBUG_TOSTRING_HELP(nMatched);
    OPDEBUG_TOSTRING_HELP(nModified);
    OPDEBUG_TOSTRING_HELP(ninserted);
    OPDEBUG_TOSTRING_HELP(ndeleted);
    OPDEBUG_TOSTRING_HELP_BOOL(fastmod);
    OPDEBUG_TOSTRING_HELP_BOOL(fastmodinsert);
    OPDEBUG_TOSTRING_HELP_BOOL(upsert);
    OPDEBUG_TOSTRING_HELP_BOOL(cursorExhausted);
    OPDEBUG_TOSTRING_HELP(keyUpdates);
    OPDEBUG_TOSTRING_HELP(writeConflicts);

    if (extra.len())
        s << " " << extra.str();

    if (!exceptionInfo.empty()) {
        s << " exception: " << exceptionInfo.msg;
        if (exceptionInfo.code)
            s << " code:" << exceptionInfo.code;
    }

    s << " numYields:" << curop.numYields();

    OPDEBUG_TOSTRING_HELP(nreturned);
    if (responseLength > 0) {
        s << " reslen:" << responseLength;
    }

    {
        BSONObjBuilder locks;
        lockStats.report(&locks);
        s << " locks:" << locks.obj().toString();
    }

    s << " " << executionTime << "ms";

    return s.str();
}

namespace {
/**
 * Appends {name: obj} to the provided builder.  If obj is greater than maxSize, appends a
 * string summary of obj instead of the object itself.
 */
void appendAsObjOrString(StringData name,
                         const BSONObj& obj,
                         size_t maxSize,
                         BSONObjBuilder* builder) {
    if (static_cast<size_t>(obj.objsize()) <= maxSize) {
        builder->append(name, obj);
    } else {
        // Generate an abbreviated serialization for the object, by passing false as the
        // "full" argument to obj.toString().
        const bool isArray = false;
        const bool full = false;
        std::string objToString = obj.toString(isArray, full);
        if (objToString.size() <= maxSize) {
            builder->append(name, objToString);
        } else {
            // objToString is still too long, so we append to the builder a truncated form
            // of objToString concatenated with "...".  Instead of creating a new string
            // temporary, mutate objToString to do this (we know that we can mutate
            // characters in objToString up to and including objToString[maxSize]).
            objToString[maxSize - 3] = '.';
            objToString[maxSize - 2] = '.';
            objToString[maxSize - 1] = '.';
            builder->append(name, StringData(objToString).substr(0, maxSize));
        }
    }
}
}  // namespace

#define OPDEBUG_APPEND_NUMBER(x) \
    if (x != -1)                 \
    b.appendNumber(#x, (x))
#define OPDEBUG_APPEND_BOOL(x) \
    if (x)                     \
    b.appendBool(#x, (x))
void OpDebug::append(const CurOp& curop,
                     const SingleThreadedLockStats& lockStats,
                     BSONObjBuilder& b) const {
    const size_t maxElementSize = 50 * 1024;

    b.append("op", iscommand ? "command" : opToString(op));
    b.append("ns", ns);

    if (!query.isEmpty()) {
        appendAsObjOrString(iscommand ? "command" : "query", query, maxElementSize, &b);
    } else if (!iscommand && curop.haveQuery()) {
        appendAsObjOrString("query", curop.query(), maxElementSize, &b);
    }

    if (!updateobj.isEmpty()) {
        appendAsObjOrString("updateobj", updateobj, maxElementSize, &b);
    }

    const bool moved = (nmoved >= 1);

    OPDEBUG_APPEND_NUMBER(cursorid);
    OPDEBUG_APPEND_NUMBER(ntoreturn);
    OPDEBUG_APPEND_NUMBER(ntoskip);
    OPDEBUG_APPEND_BOOL(exhaust);

    OPDEBUG_APPEND_NUMBER(nscanned);
    OPDEBUG_APPEND_NUMBER(nscannedObjects);
    OPDEBUG_APPEND_BOOL(idhack);
    OPDEBUG_APPEND_BOOL(scanAndOrder);
    OPDEBUG_APPEND_BOOL(moved);
    OPDEBUG_APPEND_NUMBER(nmoved);
    OPDEBUG_APPEND_NUMBER(nMatched);
    OPDEBUG_APPEND_NUMBER(nModified);
    OPDEBUG_APPEND_NUMBER(ninserted);
    OPDEBUG_APPEND_NUMBER(ndeleted);
    OPDEBUG_APPEND_BOOL(fastmod);
    OPDEBUG_APPEND_BOOL(fastmodinsert);
    OPDEBUG_APPEND_BOOL(upsert);
    OPDEBUG_APPEND_BOOL(cursorExhausted);
    OPDEBUG_APPEND_NUMBER(keyUpdates);
    OPDEBUG_APPEND_NUMBER(writeConflicts);
    b.appendNumber("numYield", curop.numYields());

    {
        BSONObjBuilder locks(b.subobjStart("locks"));
        lockStats.report(&locks);
    }

    if (!exceptionInfo.empty()) {
        exceptionInfo.append(b, "exception", "exceptionCode");
    }

    OPDEBUG_APPEND_NUMBER(nreturned);
    OPDEBUG_APPEND_NUMBER(responseLength);
    b.append("millis", executionTime);

    execStats.append(b, "execStats");
}

}  // namespace mongo
