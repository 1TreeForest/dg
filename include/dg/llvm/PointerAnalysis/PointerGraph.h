#ifndef LLVM_DG_POINTER_SUBGRAPH_H_
#define LLVM_DG_POINTER_SUBGRAPH_H_

#include <unordered_map>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_os_ostream.h>

#include "dg/llvm/PointerAnalysis/LLVMPointerAnalysisOptions.h"

#include "dg/PointerAnalysis/Pointer.h"
#include "dg/PointerAnalysis/PointerGraph.h"
#include "dg/PointerAnalysis/PointsToMapping.h"

namespace dg {
namespace pta {

class LLVMPointerGraphBuilder {
    PointerGraph PS{};
    // mapping from llvm values to PSNodes that contain
    // the points-to information
    PointsToMapping<const llvm::Value *> mapping;

    const llvm::Module *M;
    LLVMPointerAnalysisOptions _options;

    // flag that says whether we are building normally,
    // or the analysis is already running and we are building
    // some new parts of already built graph.
    // This is important with function pointer calls
    bool ad_hoc_building = false;
    // flag that determines whether invalidate nodes
    // should be created
    bool invalidate_nodes = false;

    bool threads_ = false;

    class PSNodesSeq {
        using NodesT = std::vector<PSNode *>;
        NodesT _nodes;
        // representant that holds the final points-to set after
        // generated by this sequence of instructions
        PSNode *_repr{nullptr};

      public:
        PSNodesSeq() = default;
        PSNodesSeq(PSNode *n) { _nodes.push_back(n); }
        PSNodesSeq(const std::initializer_list<PSNode *> &l) {
            for (auto *n : l)
                _nodes.push_back(n);
        }

        void setRepresentant(PSNode *r) { _repr = r; }
        PSNode *getRepresentant() { return _repr ? _repr : _nodes.back(); }
        const PSNode *getRepresentant() const {
            return _repr ? _repr : _nodes.back();
        }

        PSNode *getSingleNode() {
            assert(_nodes.size() == 1);
            return _nodes.front();
        }
        const PSNode *getSingleNode() const {
            assert(_nodes.size() == 1);
            return _nodes.front();
        }

        void append(PSNode *n) { _nodes.push_back(n); }
        bool empty() const { return _nodes.empty(); }

        PSNode *getFirst() {
            assert(!_nodes.empty());
            return _nodes.front();
        }
        PSNode *getLast() {
            assert(!_nodes.empty());
            return _nodes.back();
        }

        NodesT::iterator begin() { return _nodes.begin(); }
        NodesT::const_iterator begin() const { return _nodes.begin(); }
        NodesT::iterator end() { return _nodes.end(); }
        NodesT::const_iterator end() const { return _nodes.end(); }
    };

    class PSNodesBlock {
        using NodesT = std::vector<PSNodesSeq *>;
        NodesT _nodes;

      public:
        PSNodesBlock() = default;
        PSNodesBlock(PSNodesSeq *s) { append(s); }
        void append(PSNodesSeq *s) { _nodes.push_back(s); }
        bool empty() const { return _nodes.empty(); }

        PSNodesSeq &getFirst() {
            assert(!empty());
            return *_nodes.front();
        }
        PSNodesSeq &getLast() {
            assert(!empty());
            return *_nodes.back();
        }
        PSNode *getFirstNode() {
            assert(!empty());
            return _nodes.front()->getFirst();
        }
        PSNode *getLastNode() {
            assert(!empty());
            return _nodes.back()->getLast();
        }

        NodesT::iterator begin() { return _nodes.begin(); }
        NodesT::const_iterator begin() const { return _nodes.begin(); }
        NodesT::iterator end() { return _nodes.end(); }
        NodesT::const_iterator end() const { return _nodes.end(); }
    };

    struct FuncGraph {
        // reachable LLVM block (those block for which we built the
        // instructions)
        std::map<const llvm::BasicBlock *, PSNodesBlock> llvmBlocks{};
        bool has_structure{false};

        FuncGraph() = default;
        FuncGraph(const FuncGraph &) = delete;

        void
        blockAddSuccessors(std::set<const llvm::BasicBlock *> &found_blocks,
                           LLVMPointerGraphBuilder::PSNodesBlock &blk,
                           const llvm::BasicBlock &block);
    };

    // helper function that add CFG edges between instructions
    static void PSNodesSequenceAddSuccessors(PSNodesSeq &seq) {
        if (seq.empty())
            return;

        PSNode *last = nullptr;
        for (auto *nd : seq) {
            if (last)
                last->addSuccessor(nd);

            last = nd;
        }
    }

    static void PSNodesBlockAddSuccessors(PSNodesBlock &blk,
                                          bool withSeqEdges = false) {
        if (blk.empty())
            return;

        PSNodesSeq *last = nullptr;
        for (auto *seq : blk) {
            if (withSeqEdges)
                PSNodesSequenceAddSuccessors(*seq);

            if (last)
                last->getLast()->addSuccessor(seq->getFirst());

            last = seq;
        }
    }

    std::unordered_map<const llvm::Function *, FuncGraph> _funcInfo;

    // build pointer state subgraph for given graph
    // \return   root node of the graph
    PointerSubgraph &buildFunction(const llvm::Function &F);
    PSNodesSeq &buildInstruction(const llvm::Instruction & /*Inst*/);

    PSNodesBlock buildPointerGraphBlock(const llvm::BasicBlock &block,
                                        PointerSubgraph *parent);

    void buildArguments(const llvm::Function &F, PointerSubgraph *parent);
    PSNodesBlock buildArgumentsStructure(const llvm::Function &F);
    void buildGlobals();

    // add edges that are derived from CFG to the subgraph
    void addProgramStructure();
    void addProgramStructure(const llvm::Function *F, PointerSubgraph &subg);
    void blockAddCalls(const llvm::BasicBlock &block);

    static void addCFGEdges(const llvm::Function *F,
                            LLVMPointerGraphBuilder::FuncGraph &finfo,
                            PSNode *lastNode);

    static PSNode *connectArguments(const llvm::Function *F,
                                    PSNodesBlock &argsBlk,
                                    PointerSubgraph &subg);

    // map of all nodes we created - use to look up operands
    std::unordered_map<const llvm::Value *, PSNodesSeq> nodes_map;
    // map of all built subgraphs - the value type is a pair (root, return)
    std::unordered_map<const llvm::Function *, PointerSubgraph *> subgraphs_map;

    std::vector<PSNodeFork *> forkNodes;
    std::vector<PSNodeJoin *> joinNodes;

  public:
    const PointerGraph *getPS() const { return &PS; }

    inline bool threads() const { return threads_; }

    LLVMPointerGraphBuilder(const llvm::Module *m,
                            const LLVMPointerAnalysisOptions &opts)
            : M(m), _options(opts), threads_(opts.threads) {}

    PointerGraph *buildLLVMPointerGraph();

    bool validateSubgraph(bool no_connectivity = false) const;

    void setAdHocBuilding(bool adHoc) { ad_hoc_building = adHoc; }

    PSNodesSeq &createFuncptrCall(const llvm::CallInst *CInst,
                                  const llvm::Function *F);

    static bool callIsCompatible(PSNode *call, PSNode *func);

    // Insert a call of a function into an already existing graph.
    // The call will be inserted betwee the callsite and
    // the return from the call nodes.
    void insertFunctionCall(PSNode *callsite, PSNode *called);
    void insertPthreadCreateByPtrCall(PSNode *callsite);
    void insertPthreadJoinByPtrCall(PSNode *callsite);

    PSNodeFork *createForkNode(const llvm::CallInst *CInst,
                               PSNode * /*callNode*/);
    PSNodeJoin *createJoinNode(const llvm::CallInst *CInst,
                               PSNode * /*callNode*/);
    PSNodesSeq createPthreadCreate(const llvm::CallInst *CInst);
    PSNodesSeq createPthreadJoin(const llvm::CallInst *CInst);
    PSNodesSeq createPthreadExit(const llvm::CallInst *CInst);

    bool addFunctionToFork(PSNode *function, PSNodeFork *forkNode);
    bool addFunctionToJoin(PSNode *function, PSNodeJoin *joinNode);

    bool matchJoinToRightCreate(PSNode *joinNode);
    // let the user get the nodes map, so that we can
    // map the points-to informatio back to LLVM nodes
    const std::unordered_map<const llvm::Value *, PSNodesSeq> &
    getNodesMap() const {
        return nodes_map;
    }

    std::vector<PSNode *> getFunctionNodes(const llvm::Function *F) const;

    // this is the same as the getNode, but it creates ConstantExpr
    PSNode *getPointsToNode(const llvm::Value *val) {
        PSNode *n = getPointsToNodeOrNull(val);
        if (!n)
            n = getConstant(val);

        return n;
    }

    std::vector<PSNode *> getPointsToFunctions(const llvm::Value *calledValue);

    std::vector<PSNodeJoin *> &getJoins() { return joinNodes; }
    std::vector<PSNodeFork *> &getForks() { return forkNodes; }
    const std::vector<PSNodeJoin *> &getJoins() const { return joinNodes; }
    const std::vector<PSNodeFork *> &getForks() const { return forkNodes; }

    PSNodeJoin *findJoin(const llvm::CallInst *callInst) const;
    void setInvalidateNodesFlag(bool value) {
        assert(PS.getEntry() == nullptr &&
               "This function must be called before building PS");
        this->invalidate_nodes = value;
    }

    void composeMapping(PointsToMapping<PSNode *> &&rhs) {
        mapping.compose(std::move(rhs));
    }

    PointerSubgraph *getSubgraph(const llvm::Function * /*F*/);

  private:
    // create subgraph of function @F (the nodes)
    // and call+return nodes to/from it. This function
    // won't add the CFG edges if not 'ad_hoc_building'
    // is set to true
    PSNodesSeq &createCallToFunction(const llvm::CallInst * /*CInst*/,
                                     const llvm::Function * /*F*/);

    PSNode *getPointsToNodeOrNull(const llvm::Value *val) {
        // if we have a mapping for this node (e.g. the original
        // node was optimized away and replaced by mapping),
        // return it
        if (auto *mp = mapping.get(val))
            return mp;
        if (auto *nds = getNodes(val)) {
            // otherwise get the representant of the built nodes
            return nds->getRepresentant();
        }

        // not built!
        return nullptr;
    }

    // get the built nodes for this value or null
    PSNodesSeq *getNodes(const llvm::Value *val) {
        auto it = nodes_map.find(val);
        if (it == nodes_map.end())
            return nullptr;

        // the node corresponding to the real llvm value
        // is always the last
        return &it->second;
    }

    PSNodesSeq &addNode(const llvm::Value *val, PSNode *node) {
        assert(nodes_map.find(val) == nodes_map.end());
        auto it = nodes_map.emplace(val, node);
        node->setUserData(const_cast<llvm::Value *>(val));

        return it.first->second;
    }

    PSNodesSeq &addNode(const llvm::Value *val, PSNodesSeq seq) {
        assert(nodes_map.find(val) == nodes_map.end());
        seq.getRepresentant()->setUserData(const_cast<llvm::Value *>(val));
        auto it = nodes_map.emplace(val, std::move(seq));

        return it.first->second;
    }

    bool isRelevantInstruction(const llvm::Instruction &Inst);

    PSNodesSeq &createAlloc(const llvm::Instruction *Inst);
    PSNode *createDynamicAlloc(const llvm::CallInst *CInst,
                               AllocationFunction type);
    PSNodesSeq &createStore(const llvm::Instruction *Inst);
    PSNodesSeq &createLoad(const llvm::Instruction *Inst);
    PSNodesSeq &createGEP(const llvm::Instruction *Inst);
    PSNodesSeq &createSelect(const llvm::Instruction *Inst);
    PSNodesSeq &createPHI(const llvm::Instruction *Inst);
    PSNodesSeq &createCast(const llvm::Instruction *Inst);
    PSNodesSeq &createReturn(const llvm::Instruction *Inst);
    PSNodesSeq &createPtrToInt(const llvm::Instruction *Inst);
    PSNodesSeq &createIntToPtr(const llvm::Instruction *Inst);
    PSNodesSeq &createAsm(const llvm::Instruction *Inst);
    PSNodesSeq &createInsertElement(const llvm::Instruction *Inst);
    PSNodesSeq &createExtractElement(const llvm::Instruction *Inst);
    PSNodesSeq &createAtomicRMW(const llvm::Instruction *Inst);
    PSNodesSeq &createConstantExpr(const llvm::ConstantExpr *CE);

    PSNode *createInternalLoad(const llvm::Instruction *Inst);
    PSNodesSeq &createIrrelevantInst(const llvm::Value *,
                                     bool build_uses = false);
    PSNodesSeq &createArgument(const llvm::Argument * /*farg*/);
    void createIrrelevantUses(const llvm::Value *val);

    PSNodesSeq &createAdd(const llvm::Instruction *Inst);
    PSNodesSeq &createArithmetic(const llvm::Instruction *Inst);
    PSNodesSeq &createUnknown(const llvm::Value *val);
    PSNode *createFree(const llvm::Instruction *Inst);
    PSNode *createLifetimeEnd(const llvm::Instruction *Inst);

    PSNode *getOperand(const llvm::Value *val);
    PSNode *tryGetOperand(const llvm::Value *val);
    PSNode *getConstant(const llvm::Value *val);
    Pointer handleConstantGep(const llvm::GetElementPtrInst *GEP);
    Pointer handleConstantBitCast(const llvm::BitCastInst *BC);
    Pointer handleConstantPtrToInt(const llvm::PtrToIntInst *P2I);
    Pointer handleConstantIntToPtr(const llvm::IntToPtrInst *I2P);
    Pointer handleConstantAdd(const llvm::Instruction *Inst);
    Pointer handleConstantArithmetic(const llvm::Instruction *Inst);
    Pointer getConstantExprPointer(const llvm::ConstantExpr *CE);

    void checkMemSet(const llvm::Instruction *Inst);
    void addPHIOperands(PSNode *node, const llvm::PHINode *PHI);
    void addPHIOperands(const llvm::Function &F);
    void addArgumentOperands(const llvm::Function *F, PSNode *arg,
                             unsigned idx);
    void addArgumentOperands(const llvm::CallInst *CI, PSNode *arg,
                             unsigned idx);
    void addArgumentOperands(const llvm::CallInst &CI, PSNode &node);
    void addArgumentsOperands(const llvm::Function *F,
                              const llvm::CallInst *CI = nullptr,
                              unsigned index = 0);
    void addVariadicArgumentOperands(const llvm::Function *F, PSNode *arg);
    void addVariadicArgumentOperands(const llvm::Function *F,
                                     const llvm::CallInst *CI, PSNode *arg);

    void addReturnNodesOperands(const llvm::Function *F, PointerSubgraph &subg,
                                PSNode *callNode = nullptr);

    static void addReturnNodeOperand(PSNode *callNode, PSNode *ret);
    void addReturnNodeOperand(const llvm::Function *F, PSNode *op);
    void addInterproceduralOperands(const llvm::Function *F,
                                    PointerSubgraph &subg,
                                    const llvm::CallInst *CI = nullptr,
                                    PSNode *callNode = nullptr);
    void addInterproceduralPthreadOperands(const llvm::Function *F,
                                           const llvm::CallInst *CI = nullptr);

    PSNodesSeq &createExtract(const llvm::Instruction *Inst);
    PSNodesSeq &createCall(const llvm::Instruction *Inst);
    PSNodesSeq &createFunctionCall(const llvm::CallInst *,
                                   const llvm::Function *);
    PSNodesSeq createUndefFunctionCall(const llvm::CallInst * /*CInst*/,
                                       const llvm::Function * /*func*/);
    PSNodesSeq &createFuncptrCall(const llvm::CallInst * /*CInst*/,
                                  const llvm::Value * /*calledVal*/);

    PointerSubgraph &createOrGetSubgraph(const llvm::Function * /*F*/);
    PointerSubgraph &getAndConnectSubgraph(const llvm::Function *F,
                                           const llvm::CallInst *CInst,
                                           PSNode *callNode);

    void handleGlobalVariableInitializer(const llvm::Constant *C,
                                         PSNodeAlloc *node,
                                         uint64_t offset = 0);

    PSNode *createMemTransfer(const llvm::IntrinsicInst *Inst);
    PSNodesSeq createMemSet(const llvm::Instruction * /*Inst*/);
    PSNodesSeq createDynamicMemAlloc(const llvm::CallInst *CInst,
                                     AllocationFunction type);
    PSNodesSeq createRealloc(const llvm::CallInst *CInst);
    PSNode *createUnknownCall();
    PSNodesSeq createIntrinsic(const llvm::Instruction *Inst);
    PSNodesSeq createVarArg(const llvm::IntrinsicInst *Inst);
};

/// --------------------------------------------------------
// Helper functions
/// --------------------------------------------------------
inline bool isRelevantIntrinsic(const llvm::Function *func,
                                bool invalidate_nodes) {
    using namespace llvm;

    switch (func->getIntrinsicID()) {
    case Intrinsic::memmove:
    case Intrinsic::memcpy:
    case Intrinsic::vastart:
    case Intrinsic::stacksave:
    case Intrinsic::stackrestore:
        return true;
    case Intrinsic::lifetime_end:
        return invalidate_nodes;
    // case Intrinsic::memset:
    default:
        return false;
    }
}

inline bool isInvalid(const llvm::Value *val, bool invalidate_nodes) {
    using namespace llvm;

    if (!isa<Instruction>(val)) {
        if (!isa<Argument>(val) && !isa<GlobalValue>(val))
            return true;
    } else {
        if (isa<ICmpInst>(val) || isa<FCmpInst>(val) ||
            isa<DbgValueInst>(val) || isa<BranchInst>(val) ||
            isa<SwitchInst>(val))
            return true;

        const CallInst *CI = dyn_cast<CallInst>(val);
        if (CI) {
            const Function *F = CI->getCalledFunction();
            if (F && F->isIntrinsic() &&
                !isRelevantIntrinsic(F, invalidate_nodes))
                return true;
        }
    }

    return false;
}

} // namespace pta
} // namespace dg

#endif
