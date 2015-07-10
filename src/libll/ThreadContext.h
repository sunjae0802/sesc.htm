#ifndef THREADCONTEXT_H
#define THREADCONTEXT_H

#include <stdint.h>
#include <cstring>
#include <vector>
#include <set>
#include "Snippets.h"
#include "libemul/AddressSpace.h"
#include "libemul/SignalHandling.h"
#include "libemul/FileSys.h"
#include "libemul/InstDesc.h"
#include "libemul/LinuxSys.h"

#if (defined TM)
#include "libTM/TMContext.h"
#endif

enum TMBeginSubtype {
    TM_BEGIN_INVALID,
    TM_BEGIN_REGULAR,
    TM_BEGIN_IGNORE,
    TM_BEGIN_NACKED,
    TM_COMPLETE_ABORT
};
enum TMCommitSubtype {
    TM_COMMIT_INVALID,
    TM_COMMIT_REGULAR,
    TM_COMMIT_ABORTED,
};

// Contains information for instructions that are at function boundaries about
// the function itself. Entry point instructions (calls) contain arguments and
// the return address, exit point instructions (returns) contain the return value.
struct FuncBoundaryData {
    static FuncBoundaryData createCall(enum FuncName name,
            uint32_t retA, uint32_t a0, uint32_t a1) {
        return FuncBoundaryData(name, true, retA, 0, a0, a1);
    }
    static FuncBoundaryData createRet(enum FuncName name,
            uint32_t retV) {
        return FuncBoundaryData(name, false, 0, retV, 0, 0);
    }
    FuncBoundaryData(enum FuncName name, bool call,
            uint32_t retA, uint32_t retV, uint32_t a0, uint32_t a1):
        funcName(name), isCall(call), ra(retA), rv(retV), arg0(a0), arg1(a1) {}

    enum FuncName funcName;
    bool isCall;        // Call if true, return if false
    uint32_t ra;
    uint32_t rv;
    uint32_t arg0;
    uint32_t arg1;
};

struct TimeTrackerStats {
    TimeTrackerStats(): totalLengths(0), totalCommitted(0), totalAborted(0),
        totalWait(0), totalMutexWait(0), totalMutex(0)
    {}
    void print() const;
    void sum(const TimeTrackerStats& other);
    uint64_t totalLengths;
    uint64_t totalCommitted;
    uint64_t totalAborted;
    uint64_t totalWait;
    uint64_t totalMutexWait;
    uint64_t totalMutex;
};

// Represents a subregion within an AtomicRegion
class AtomicSubregion {
public:
    enum SubregionType {
        SR_INVALID,
        SR_WAIT,
        SR_TRANSACTION,
        SR_CRITICAL_SECTION
    };
    AtomicSubregion(enum SubregionType t, Time_t s):
            type(t), startAt(s), endAt(0) {}
    virtual ~AtomicSubregion() {}

    void markEnd(Time_t e) {
        endAt = e;
    }
    enum SubregionType type;
    Time_t      startAt;
    Time_t      endAt;
};

class TMSubregion: public AtomicSubregion {
public:
    TMSubregion(Time_t s):
            AtomicSubregion(SR_TRANSACTION, s), aborted(false) {}
    void markAborted(Time_t at) {
        aborted = true;
        markEnd(at);
    }
    bool        aborted;
};

class CSSubregion: public AtomicSubregion {
public:
    CSSubregion(Time_t s):
            AtomicSubregion(SR_CRITICAL_SECTION, s), acquiredAt(0) {}
    void markAcquired(Time_t at) {
        acquiredAt = at;
    }
    Time_t      acquiredAt;
};

// Used to track timing statistics of atomic regions (between tm_begin and tm_end).
// All timing is done at retire-time of DInsts.
struct AtomicRegionStats {
    AtomicRegionStats() { clear(); }
    void init(VAddr pc, Time_t at) {
        clear();
        startPC = pc;
        startAt = at;
    }
    void markEnd(Time_t at) {
        endAt = at;
    }
    void clear();
    void markRetireFuncBoundary(DInst* dinst, FuncBoundaryData& funcData);
    void markRetireTM(DInst* dinst);
    void calculate(TimeTrackerStats* p_stats);
    typedef std::vector<AtomicSubregion*> Subregions;

    VAddr               startPC;
    Time_t              startAt;
    Time_t              endAt;
    AtomicSubregion*    p_current;
    Subregions          subregions;
};

// Use this define to debug the simulated application
// It enables call stack tracking
//#define DEBUG_BENCH

class ThreadContext : public GCObject {
public:
    typedef SmartPtr<ThreadContext> pointer;
    static bool ff;
    static bool simDone;
	static int64_t finalSkip;
    static Time_t resetTS;
    std::vector<FuncBoundaryData> funcData;

    uint64_t nRetiredInsts;
    AtomicRegionStats       currentRegion;
    static TimeTrackerStats timeTrackerStats;
    TimeTrackerStats        myTimeStats;
private:
    void initialize(bool child);
	void cleanup();
    void initTrace();
    void closeTrace();
    typedef std::vector<pointer> ContextVector;
    // Static variables
    static ContextVector pid2context;


	// TM
#if (defined TM)
    // Unique transaction identifier
    uint64_t tmUtid;
    // Stall thread if we get a NACK
    Time_t  tmStallUntil;
    // Saved thread context
    TMContext *tmContext;
    // Where user had called HTM "instructions"
    VAddr tmCallsite;
    // User-passed HTM command arg
    uint32_t tmArg;
    // User-passed abort arg
    uint32_t tmAbortArg;
    // The IAddr when we found out TM has aborted
    VAddr  tmAbortIAddr;
    // Number of transactional writes done on tm.commit
    size_t      tmNumWrites;
    // Cycles for stalling retire of a tm instruction
    uint32_t    tmLat;
    TMBeginSubtype tmBeginSubtype;
    TMCommitSubtype tmCommitSubtype;
#endif

    // Memory Mapping

    // Lower and upper bound for stack addresses in this thread
    VAddr myStackAddrLb;
    VAddr myStackAddrUb;

    // Local Variables
private:
    int32_t pid;		// process id

    // Execution mode of this thread
    ExecMode execMode;
    // Register file(s)
    RegVal regs[NumOfRegs];
    // Address space for this thread
    AddressSpace::pointer addressSpace;
    // Instruction pointer
    VAddr     iAddr;
    // Instruction descriptor
    InstDesc *iDesc;
    // Virtual address generated by the last memory access instruction
    VAddr     dAddr;
    // Flag indicating whether the last memory access was a hit in L1
    bool      l1Hit;
    size_t    nDInsts;

    // HACK to balance calls/returns
    typedef void (*retHandler_t)(InstDesc *, ThreadContext *);
    std::vector<std::pair<VAddr, retHandler_t> > retHandlers;
    std::vector<std::pair<VAddr, retHandler_t> > retHandlersSaved;

public:
    void markRetire(DInst* dinst);
    void saveCallRetStack() {
        retHandlersSaved = retHandlers;
    }
    void restoreCallRetStack() {
        retHandlers = retHandlersSaved;
    }
    void addCall(VAddr ra, retHandler_t handler) {
        retHandlers.push_back(std::make_pair(ra, handler));
    }
    void handleReturns(VAddr destIAddr, InstDesc *inst) {
        while(!retHandlers.empty()) {
            std::pair<VAddr, retHandler_t> handler_ra = retHandlers.back();
            if(handler_ra.first == destIAddr) {
                handler_ra.second(inst, this);
                retHandlers.pop_back();
            } else {
                break;
            }
        }
    }
    bool retsEmpty() const { return retHandlers.empty(); }

#if (defined TM)
    // Debug flag for making sure we have consistent view of SW tid and HW tid
    int32_t tmlibUserTid;

    // Transactional Helper Methods
    int getTMdepth()        const { return tmCohManager ? tmCohManager->getDepth(pid) : 0; }
    bool isInTM()           const { return getTMdepth() > 0; }
    bool isTMAborting()     const { return tmCohManager->getState(pid) == TM_ABORTING; }
    bool markedForAbort()   const { return tmCohManager->getState(pid) == TM_MARKABORT; }
    bool tmSuspended()      const { return tmCohManager->getState(pid) == TM_SUSPENDED; }

    TMContext* getTMContext() const { return tmContext; }
    void setTMContext(TMContext* newTMContext) { tmContext = newTMContext; }

    void setTMCallsite(VAddr ra) { tmCallsite = ra; }
    VAddr getTMCallsite() const { return tmCallsite; }

    TMBeginSubtype getTMBeginSubtype() const { return tmBeginSubtype; }
    void clearTMBeginSubtype() { tmBeginSubtype = TM_BEGIN_INVALID; }
    TMCommitSubtype getTMCommitSubtype() const { return tmCommitSubtype; }
    void clearTMCommitSubtype() { tmCommitSubtype = TM_COMMIT_INVALID; }

    // TM getters
    uint32_t getTMArg()       const { return tmArg; }
    uint32_t getTMAbortIAddr() const{ return tmAbortIAddr; }
    uint32_t getTMAbortArg()  const { return tmAbortArg; }
    uint32_t getTMLat()       const { return tmLat; }

    // Transactional Methods
    TMBCStatus beginTransaction(InstDesc* inst);
    TMBCStatus commitTransaction(InstDesc* inst);
    TMBCStatus abortTransaction(InstDesc* inst, TMAbortType_e abortType);
    TMBCStatus abortTransaction(InstDesc* inst);

    TMBCStatus userBeginTM(InstDesc* inst, uint32_t arg) {
        tmArg = arg;
        TMBCStatus status = beginTransaction(inst);
        return status;
    }
    TMBCStatus userCommitTM(InstDesc* inst, uint32_t arg) {
        tmArg = arg;
        TMBCStatus status = commitTransaction(inst);
        return status;
    }
    void userAbortTM(InstDesc* inst, uint32_t arg) {
        tmArg       = arg;
        tmAbortArg  = tmArg;
        abortTransaction(inst, TM_ATYPE_USER);
    }
    void completeAbort(InstDesc* inst);
    uint32_t getBeginRV(TMBCStatus status);
    uint32_t getAbortRV(TMBCStatus status);
    void beginFallback(uint32_t pFallbackMutex);
    void completeFallback();

    // memop NACK handling methods
    void startRetryTimer() {
        startStalling(tmCohManager->getNackRetryStallCycles());
    }
    void startStalling(TimeDelta_t amount) {
        tmStallUntil = globalClock + amount;
    }
    bool checkStall() const {
        return tmStallUntil != 0 && tmStallUntil >= globalClock;
    }
#endif
    static inline int32_t getPidUb(void) {
        return pid2context.size();
    }
    void setMode(ExecMode mode);
    inline ExecMode getMode(void) const {
        return execMode;
    }

    inline const void *getReg(RegName name) const {
        return &(regs[name]);
    }
    inline void *getReg(RegName name) {
        return &(regs[name]);
    }
    void clearRegs(void) {
        memset(regs,0,sizeof(regs));
    }

#if (defined TM)
	void setReg(RegName name, RegVal val) {
		regs[name] = val;
	}
#endif
    // Returns the pid of the context
    Pid_t getPid(void) const {
        return pid;
    }

    void copy(const ThreadContext *src);

    static ThreadContext *getContext(Pid_t pid);

    static ThreadContext *getMainThreadContext(void) {
        return &(*(pid2context[0]));
    }

    // BEGIN Memory Mapping
    bool isValidDataVAddr(VAddr vaddr) const {
        return canRead(vaddr,1)||canWrite(vaddr,1);
    }

    ThreadContext(FileSys::FileSys *fileSys);
    ThreadContext(ThreadContext &parent, bool cloneParent,
                  bool cloneFileSys, bool newNameSpace,
                  bool cloneFiles, bool cloneSighand,
                  bool cloneVm, bool cloneThread,
                  SignalID sig, VAddr clearChildTid);
    ~ThreadContext();

    ThreadContext *createChild(bool shareAddrSpace, bool shareSigTable, bool shareOpenFiles, SignalID sig);
    void setAddressSpace(AddressSpace *newAddressSpace);
    AddressSpace *getAddressSpace(void) const {
        I(addressSpace);
        return addressSpace;
    }
    inline void setStack(VAddr stackLb, VAddr stackUb) {
        myStackAddrLb=stackLb;
        myStackAddrUb=stackUb;
    }
    inline VAddr getStackAddr(void) const {
        return myStackAddrLb;
    }
    inline VAddr getStackSize(void) const {
        return myStackAddrUb-myStackAddrLb;
    }

    inline InstDesc *virt2inst(VAddr vaddr) {
        InstDesc *inst=addressSpace->virtToInst(vaddr);
        if(!inst) {
            addressSpace->createTrace(this,vaddr);
            inst=addressSpace->virtToInst(vaddr);
        }
        return inst;
    }

    bool isLocalStackData(VAddr addr) const {
        return (addr>=myStackAddrLb)&&(addr<myStackAddrUb);
    }

    VAddr getStackTop() const {
        return myStackAddrLb;
    }
    // END Memory Mapping

    inline InstDesc *getIDesc(void) const {
        return iDesc;
    }
    inline void updIDesc(ssize_t ddiff) {
        I((ddiff>=-1)&&(ddiff<4));
        iDesc+=ddiff;
    }
    inline VAddr getIAddr(void) const {
        return iAddr;
    }
    inline void setIAddr(VAddr addr) {
        iAddr=addr;
        iDesc=iAddr?virt2inst(addr):0;
    }
    inline void updIAddr(ssize_t adiff, ssize_t ddiff) {
        I((ddiff>=-1)&&(ddiff<4));
        I((adiff>=-4)&&(adiff<=8));
        iAddr+=adiff;
        iDesc+=ddiff;
    }
    inline VAddr getDAddr(void) const {
        return dAddr;
    }
    inline void setDAddr(VAddr addr) {
        dAddr=addr;
    }
    inline bool getL1Hit(void) const {
        return l1Hit;
    }
    inline void setL1Hit(bool hit) {
        l1Hit = hit;
    }
    inline void addDInst(void) {
        nDInsts++;
    }
    inline void delDInst(void) {
        nDInsts--;
    }
    inline size_t getNDInsts(void) {
        return nDInsts;
    }
    static inline int32_t nextReady(int32_t startPid) {
        int32_t foundPid=startPid;
        do {
            if(foundPid==(int)(pid2context.size()))
                foundPid=0;
            ThreadContext *context=pid2context[foundPid];
            if(context&&(!context->isSuspended())&&(!context->isExited()))
                return foundPid;
            foundPid++;
        } while(foundPid!=startPid);
        return -1;
    }
    inline bool skipInst(void);
    static int64_t skipInsts(int64_t skipCount);
#if (defined HAS_MEM_STATE)
    inline const MemState &getState(VAddr addr) const {
        return addressSpace->getState(addr);
    }
    inline MemState &getState(VAddr addr) {
        return addressSpace->getState(addr);
    }
#endif
    inline bool canRead(VAddr addr, size_t len) const {
        return addressSpace->canRead(addr,len);
    }
    inline bool canWrite(VAddr addr, size_t len) const {
        return addressSpace->canWrite(addr,len);
    }
    void    writeMemFromBuf(VAddr addr, size_t len, const void *buf);
//  ssize_t writeMemFromFile(VAddr addr, size_t len, int32_t fd, bool natFile, bool usePread=false, off_t offs=0);
    void    writeMemWithByte(VAddr addr, size_t len, uint8_t c);
    void    readMemToBuf(VAddr addr, size_t len, void *buf);
//  ssize_t readMemToFile(VAddr addr, size_t len, int32_t fd, bool natFile);
    ssize_t readMemString(VAddr stringVAddr, size_t maxSize, char *dstStr);
    template<class T>
    inline void readMemTM(VAddr addr, T oval, T* p_val) {
        if(tmContext == NULL) {
            fail("tmContext is NULL");
        }
        tmContext->cacheAccess<T>(addr, oval, p_val);
    }
    template<class T>
    inline T readMemRaw(VAddr addr) {
        if(sizeof(T)>sizeof(MemAlignType)) {
            fail("ThreadContext:writeMemRaw with a too-large type\n");
//      T tmp;
//      I(canRead(addr,sizeof(T)));
//      readMemToBuf(addr,sizeof(T),&tmp);
//      return tmp;
        }
//    for(size_t i=0;i<(sizeof(T)+MemState::Granularity-1)/MemState::Granularity;i++)
//      if(getState(addr+i*MemState::Granularity).st==0)
//        fail("Uninitialized read found\n");


        if(addressSpace->canRead(addr) == false) {
            fail("%d reading from non-readable page\n", pid);
        }
        return addressSpace->read<T>(addr);
    }
    template<class T>
    inline void writeMemTM(VAddr addr, const T &val) {
        if(tmContext == NULL) {
            fail("tmContext is NULL");
        }
        tmContext->cacheWrite<T>(addr, val);
    }
    template<class T>
    inline void writeMemRaw(VAddr addr, const T &val) {
        //   if((addr>=0x4d565c)&&(addr<0x4d565c+12)){
        //     I(0);
        //     I(iAddr!=0x004bb428);
        //     I(iAddr!=0x004c8604);
        //     const char *fname="Unknown";
        //     if(iAddr)
        //       fname=getAddressSpace()->getFuncName(getAddressSpace()->getFuncAddr(iAddr));
        //     printf("Write 0x%08x to 0x%08x at 0x%08x in %s\n",
        //       val,addr,iAddr,fname);
        //   }
        if(sizeof(T)>sizeof(MemAlignType)) {
            fail("ThreadContext:writeMemRaw with a too-large type\n");
//      if(!canWrite(addr,sizeof(val)))
//	return false;
//      writeMemFromBuf(addr,sizeof(val),&val);
//      return true;
        }
//    for(size_t i=0;i<(sizeof(T)+MemState::Granularity-1)/MemState::Granularity;i++)
//      getState(addr+i*MemState::Granularity).st=1;

        if(addressSpace->canWrite(addr) == false) {
            fail("%d writing to non-writeable page\n", pid);
        }
        addressSpace->write<T>(addr,val);
    }
#if (defined DEBUG_BENCH)
    VAddr readMemWord(VAddr addr);
#endif

    //
    // File system
    //
private:
    FileSys::FileSys::pointer fileSys;
    FileSys::OpenFiles::pointer openFiles;
public:
    FileSys::FileSys *getFileSys(void) const {
        return fileSys;
    }
    FileSys::OpenFiles *getOpenFiles(void) const {
        return openFiles;
    }

    //
    // Signal handling
    //
private:
    SignalTable::pointer sigTable;
    SignalSet   sigMask;
    SignalQueue maskedSig;
    SignalQueue readySig;
    bool        suspSig;
public:
    void setSignalTable(SignalTable *newSigTable) {
        sigTable=newSigTable;
    }
    SignalTable *getSignalTable(void) const {
        return sigTable;
    }
    void suspend(void);
    void signal(SigInfo *sigInfo);
    void resume(void);
    const SignalSet &getSignalMask(void) const {
        return sigMask;
    }
    void setSignalMask(const SignalSet &newMask) {
        sigMask=newMask;
        for(size_t i=0; i<maskedSig.size(); i++) {
            SignalID sig=maskedSig[i]->signo;
            if(!sigMask.test(sig)) {
                readySig.push_back(maskedSig[i]);
                maskedSig[i]=maskedSig.back();
                maskedSig.pop_back();
            }
        }
        for(size_t i=0; i<readySig.size(); i++) {
            SignalID sig=readySig[i]->signo;
            if(sigMask.test(sig)) {
                maskedSig.push_back(readySig[i]);
                readySig[i]=readySig.back();
                readySig.pop_back();
            }
        }
        if((!readySig.empty())&&suspSig)
            resume();
    }
    bool hasReadySignal(void) const {
        return !readySig.empty();
    }
    SigInfo *nextReadySignal(void) {
        I(hasReadySignal());
        SigInfo *sigInfo=readySig.back();
        readySig.pop_back();
        return sigInfo;
    }

    // System state

    LinuxSys *mySystem;
    LinuxSys *getSystem(void) const {
        return mySystem;
    }

    // Parent/Child relationships
private:
    typedef std::set<int> IntSet;
    // Thread id of this thread
    int32_t tid;
    // tid of the thread group leader
    int32_t tgid;
    // This set is empty for threads that are not thread group leader
    // In a thread group leader, this set contains the other members of the thread group
    IntSet tgtids;

    // Process group Id is the PId of the process group leader
    int32_t pgid;

    int parentID;
    IntSet childIDs;
    // Signal sent to parent when this thread dies/exits
    SignalID  exitSig;
    // Futex to clear when this thread dies/exits
    VAddr clear_child_tid;
    // Robust list head pointer
    VAddr robust_list;
public:
    int32_t gettgid(void) const {
        return tgid;
    }
    size_t gettgtids(int tids[], size_t slots) const {
        IntSet::const_iterator it=tgtids.begin();
        for(size_t i=0; i<slots; i++,it++)
            tids[i]=*it;
        return tgtids.size();
    }
    int32_t gettid(void) const {
        return tid;
    }
    int32_t getpgid(void) const {
        return pgid;
    }
    int getppid(void) const {
        return parentID;
    }
    void setRobustList(VAddr headptr) {
        robust_list=headptr;
    }
    void setTidAddress(VAddr tidptr) {
        clear_child_tid=tidptr;
    }
    int32_t  getParentID(void) const {
        return parentID;
    }
    bool hasChildren(void) const {
        return !childIDs.empty();
    }
    bool isChildID(int32_t id) const {
        return (childIDs.find(id)!=childIDs.end());
    }
    int32_t findZombieChild(void) const;
    SignalID getExitSig(void) {
        return exitSig;
    }
private:
    bool     exited;
    int32_t      exitCode;
    SignalID killSignal;
public:
    bool isSuspended(void) const {
        return suspSig;
    }
    bool isExited(void) const {
        return exited;
    }
    int32_t getExitCode(void) const {
        return exitCode;
    }
    bool isKilled(void) const {
        return (killSignal!=SigNone);
    }
    SignalID getKillSignal(void) const {
        return killSignal;
    }
    // Exit this process
    // Returns: true if exit complete, false if process is now zombie
    bool exit(int32_t code);
    // Reap an exited process
    void reap();
    void doKill(SignalID sig) {
        I(!isExited());
        I(!isKilled());
        I(sig!=SigNone);
        killSignal=sig;
    }

    // Debugging

    class CallStackEntry {
    public:
        VAddr entry;
        VAddr ra;
        VAddr sp;
        bool  tailr;
        CallStackEntry(VAddr entry, VAddr  ra, VAddr sp, bool tailr)
            : entry(entry), ra(ra), sp(sp), tailr(tailr) {
        }
    };
    typedef std::vector<CallStackEntry> CallStack;
    CallStack callStack;

    void execCall(VAddr entry, VAddr  ra, VAddr sp);
    void execRet(VAddr entry, VAddr ra, VAddr sp);
    void dumpCallStack(void);
    void clearCallStack(void);

public:
    int32_t lockDepth;
    bool spinning;
    uint32_t s_lockRA;
    uint32_t s_lockArg;
    uint32_t s_barrierRA;
    uint32_t s_barrierArg;
    static bool inMain;

    static size_t numThreads;

    std::ofstream datafile;
    std::ofstream& getDatafile() {
        return getMainThreadContext()->datafile;
    }
    void traceFunction(DInst *dinst, FuncBoundaryData& funcData);
    void traceTM(DInst* dinst);

    void incParallel(Pid_t wpid) {
        std::cout<<"["<<globalClock<<"]   Thread "<<numThreads<<" ("<<wpid<<") Create"<<std::endl<<std::flush;
        getDatafile()<<ThreadContext::numThreads<<" 0 "<<nRetiredInsts<<' '<<globalClock<<std::endl;
        ThreadContext::numThreads++;
    }

    void decParallel(Pid_t wpid) {
        ThreadContext::numThreads--;
        std::cout<<"["<<globalClock<<"]   Thread "<<numThreads<<" ("<<wpid<<") Exit"<<std::endl<<std::flush;
        getDatafile()<<ThreadContext::numThreads<<" 1 "<<nRetiredInsts<<' '<<globalClock<<std::endl;
    }

    bool parallel;

    void incLockDepth() {
        lockDepth++;
    }
    void decLockDepth() {
        lockDepth--;
    }
    int32_t getLockDepth() const {
        return lockDepth;
    }
};

#endif // THREADCONTEXT_H
