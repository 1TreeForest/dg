#include "dg/llvm/ValueRelations/RelationsAnalyzer.h"

#include <algorithm>

namespace dg {
namespace vr {

using V = ValueRelations::V;

// ********************** points to invalidation ********************** //
bool RelationsAnalyzer::isIgnorableIntrinsic(llvm::Intrinsic::ID id) const {
    switch (id) {
    case llvm::Intrinsic::lifetime_start:
    case llvm::Intrinsic::lifetime_end:
    case llvm::Intrinsic::stacksave:
    case llvm::Intrinsic::stackrestore:
    case llvm::Intrinsic::dbg_declare:
    case llvm::Intrinsic::dbg_value:
        return true;
    default:
        return false;
    }
}

bool RelationsAnalyzer::isSafe(I inst) const {
    if (!inst->mayWriteToMemory() && !inst->mayHaveSideEffects())
        return true;

    if (auto intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(inst)) {
        if (isIgnorableIntrinsic(intrinsic->getIntrinsicID())) {
            return true;
        }
    }

    if (auto call = llvm::dyn_cast<llvm::CallInst>(inst)) {
        auto function = call->getCalledFunction();
        if (function && safeFunctions.find(function->getName().str()) !=
                                safeFunctions.end())
            return true;
    }
    return false;
}

bool RelationsAnalyzer::isDangerous(I inst) const {
    auto store = llvm::dyn_cast<llvm::StoreInst>(inst);
    if (!store) // most probably CallInst
        // unable to presume anything about such instruction
        return true;

    // if store writes to a fix location, it cannot be easily said which
    // values it affects
    if (llvm::isa<llvm::Constant>(store->getPointerOperand()))
        return true;

    return false;
}

bool RelationsAnalyzer::mayHaveAlias(const ValueRelations &graph, V val) const {
    for (auto eqval : graph.getEqual(val))
        if (mayHaveAlias(eqval))
            return true;
    return false;
}

bool RelationsAnalyzer::mayHaveAlias(V val) const {
    // if value is not pointer, we don't care whether there can be other name
    // for same value
    if (!val->getType()->isPointerTy())
        return false;

    if (llvm::isa<llvm::GetElementPtrInst>(val))
        return true;

    for (const llvm::User *user : val->users()) {
        // if value is stored, it can be accessed
        if (llvm::isa<llvm::StoreInst>(user)) {
            if (user->getOperand(0) == val)
                return true;

        } else if (llvm::isa<llvm::CastInst>(user)) {
            if (mayHaveAlias(user))
                return true;

        } else if (auto gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
            assert(gep->getPointerOperand() == val);
            return true; // TODO possible to collect here

        } else if (auto intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user)) {
            if (!isIgnorableIntrinsic(intrinsic->getIntrinsicID()) &&
                intrinsic->mayWriteToMemory())
                return true;

        } else if (auto inst = llvm::dyn_cast<llvm::Instruction>(user)) {
            if (inst->mayWriteToMemory())
                return true;
        }
    }
    return false;
}

bool RelationsAnalyzer::hasKnownOrigin(const ValueRelations &graph, V from) {
    for (auto val : graph.getEqual(from)) {
        if (hasKnownOrigin(val))
            return true;
    }
    return false;
}

bool RelationsAnalyzer::hasKnownOrigin(V from) {
    return llvm::isa<llvm::AllocaInst>(from);
}

V getGEPBase(V val) {
    if (auto gep = llvm::dyn_cast<llvm::GetElementPtrInst>(val))
        return gep->getPointerOperand();
    return nullptr;
}

bool sameBase(const ValueRelations &graph, V val1, V val2) {
    V val2orig = val2;
    while (val1) {
        val2 = val2orig;
        while (val2) {
            if (graph.are(val1, Relations::EQ,
                          val2)) // TODO compare whether indices may equal
                return true;
            val2 = getGEPBase(val2);
        }
        val1 = getGEPBase(val1);
    }
    return false;
}

bool RelationsAnalyzer::mayOverwrite(I inst, V address) const {
    assert(inst);
    assert(address);

    const ValueRelations &graph = codeGraph.getVRLocation(inst).relations;

    if (isSafe(inst))
        return false;

    if (isDangerous(inst))
        return true;

    auto store = llvm::cast<llvm::StoreInst>(inst);
    V memoryPtr = store->getPointerOperand();

    if (sameBase(graph, memoryPtr, address))
        return true;

    if (!graph.contains(address))
        return !hasKnownOrigin(address) || mayHaveAlias(address);

    if (!graph.contains(memoryPtr) || !hasKnownOrigin(graph, memoryPtr))
        return !hasKnownOrigin(graph, address) || mayHaveAlias(graph, address);

    if (mayHaveAlias(memoryPtr))
        return !hasKnownOrigin(graph, address);

    return false;
}

// ************************ operation helpers ************************* //
void RelationsAnalyzer::solvesDiffOne(ValueRelations &graph, V param,
                                      const llvm::BinaryOperator *op,
                                      Relations::Type rel) {
    std::vector<V> sample =
            graph.getDirectlyRelated(param, Relations().set(rel));
    for (V val : sample)
        assert(graph.are(param, rel, val));

    for (V val : sample)
        graph.set(op, Relations::getNonStrict(rel), val);
}

bool RelationsAnalyzer::operandsEqual(
        ValueRelations &graph, I fst, I snd,
        bool sameOrder) const { // false means checking in reverse order
    unsigned total = fst->getNumOperands();
    if (total != snd->getNumOperands())
        return false;

    for (unsigned i = 0; i < total; ++i) {
        unsigned otherI = sameOrder ? i : total - i - 1;

        if (!graph.isEqual(fst->getOperand(i), snd->getOperand(otherI)))
            return false;
    }
    return true;
}

void RelationsAnalyzer::solveByOperands(ValueRelations &graph,
                                        const llvm::BinaryOperator *operation,
                                        bool sameOrder) {
    for (auto same : structure.getInstructionSetFor(operation->getOpcode())) {
        auto sameOperation = llvm::dyn_cast<const llvm::BinaryOperator>(same);

        if (operandsEqual(graph, operation, sameOperation, sameOrder))
            graph.setEqual(operation, sameOperation);
    }
}

void RelationsAnalyzer::solveEquality(ValueRelations &graph,
                                      const llvm::BinaryOperator *operation) {
    solveByOperands(graph, operation, true);
}

void RelationsAnalyzer::solveCommutativity(
        ValueRelations &graph, const llvm::BinaryOperator *operation) {
    solveByOperands(graph, operation, false);
}

// ******************** gen from instruction ************************** //
void RelationsAnalyzer::storeGen(ValueRelations &graph,
                                 const llvm::StoreInst *store) {
    graph.setLoad(store->getPointerOperand()->stripPointerCasts(),
                  store->getValueOperand());
}

void RelationsAnalyzer::loadGen(ValueRelations &graph,
                                const llvm::LoadInst *load) {
    graph.setLoad(load->getPointerOperand()->stripPointerCasts(), load);
}

void RelationsAnalyzer::gepGen(ValueRelations &graph,
                               const llvm::GetElementPtrInst *gep) {
    if (gep->hasAllZeroIndices())
        graph.setEqual(gep, gep->getPointerOperand());

    for (auto it = graph.begin_buckets(Relations().pt());
         it != graph.end_buckets(); ++it) {
        for (V from : graph.getEqual(it->from())) {
            if (auto otherGep = llvm::dyn_cast<llvm::GetElementPtrInst>(from)) {
                if (operandsEqual(graph, gep, otherGep, true)) {
                    graph.setEqual(gep, otherGep);
                    return;
                }
            }
        }
    }
    // TODO something more?
    // indices method gives iterator over indices
}

void RelationsAnalyzer::extGen(ValueRelations &graph,
                               const llvm::CastInst *ext) {
    graph.setEqual(ext, ext->getOperand(0));
}

void solveNonConstants(ValueRelations &graph,
                       llvm::Instruction::BinaryOps opcode,
                       const llvm::BinaryOperator *op) {
    if (opcode != llvm::Instruction::Sub)
        return;

    const llvm::Constant *zero = llvm::ConstantInt::getSigned(op->getType(), 0);
    V fst = op->getOperand(0);
    V snd = op->getOperand(1);

    if (graph.isLesser(zero, snd) && graph.isLesserEqual(snd, fst))
        graph.setLesser(op, fst);
}

std::pair<llvm::Value *, llvm::ConstantInt *>
getParams(const llvm::BinaryOperator *op) {
    if (llvm::isa<llvm::ConstantInt>(op->getOperand(0))) {
        assert(!llvm::isa<llvm::ConstantInt>(op->getOperand(1)));
        if (op->getOpcode() == llvm::Instruction::Sub)
            return {nullptr, nullptr};
        return {op->getOperand(1),
                llvm::cast<llvm::ConstantInt>(op->getOperand(0))};
    }
    return {op->getOperand(0),
            llvm::cast<llvm::ConstantInt>(op->getOperand(1))};
}

void solveDifferent(ValueRelations &graph, const llvm::BinaryOperator *op) {
    auto opcode = op->getOpcode();

    V param = nullptr;
    llvm::ConstantInt *c = nullptr;
    std::tie(param, c) = getParams(op);

    if (!param)
        return;

    assert(param && c);

    Relations::Type shift;
    if ((opcode == llvm::Instruction::Add && c->isOne()) ||
        (opcode == llvm::Instruction::Sub && c->isMinusOne())) {
        shift = Relations::SLT;
    } else if ((opcode == llvm::Instruction::Add && c->isMinusOne()) ||
               (opcode == llvm::Instruction::Sub && c->isOne())) {
        shift = Relations::SGT;
    } else
        return;

    graph.set(param, shift, op);

    std::vector<V> sample =
            graph.getDirectlyRelated(param, Relations().set(shift));
    for (V val : sample)
        assert(graph.are(param, shift, val));

    for (V val : sample)
        graph.set(op, Relations::getNonStrict(shift), val);
}

void RelationsAnalyzer::opGen(ValueRelations &graph,
                              const llvm::BinaryOperator *op) {
    auto c1 = llvm::dyn_cast<llvm::ConstantInt>(op->getOperand(0));
    auto c2 = llvm::dyn_cast<llvm::ConstantInt>(op->getOperand(1));
    auto opcode = op->getOpcode();

    solveEquality(graph, op);
    if (opcode == llvm::Instruction::Add || opcode == llvm::Instruction::Mul)
        solveCommutativity(graph, op);

    if (opcode == llvm::Instruction::Mul)
        return;

    if (c1 && c2)
        return;

    if (!c1 && !c2)
        return solveNonConstants(graph, opcode, op);

    solveDifferent(graph, op);
}

void RelationsAnalyzer::remGen(ValueRelations &graph,
                               const llvm::BinaryOperator *rem) {
    assert(rem);
    const llvm::Constant *zero =
            llvm::ConstantInt::getSigned(rem->getType(), 0);

    if (!graph.isLesserEqual(zero, rem->getOperand(0)))
        return;

    graph.setLesserEqual(zero, rem);
    graph.setLesser(rem, rem->getOperand(1));
}

void RelationsAnalyzer::castGen(ValueRelations &graph,
                                const llvm::CastInst *cast) {
    if (cast->isLosslessCast() || cast->isNoopCast(module.getDataLayout()))
        graph.setEqual(cast, cast->getOperand(0));
}

// ******************** process assumption ************************** //
RelationsAnalyzer::Relation
RelationsAnalyzer::ICMPToRel(const llvm::ICmpInst *icmp, bool assumption) {
    llvm::ICmpInst::Predicate pred = assumption ? icmp->getSignedPredicate()
                                                : icmp->getInversePredicate();

    switch (pred) {
    case llvm::ICmpInst::Predicate::ICMP_EQ:
        return Relation::EQ;
    case llvm::ICmpInst::Predicate::ICMP_NE:
        return Relation::NE;
    case llvm::ICmpInst::Predicate::ICMP_ULE:
        return Relation::ULE;
    case llvm::ICmpInst::Predicate::ICMP_SLE:
        return Relation::SLE;
    case llvm::ICmpInst::Predicate::ICMP_ULT:
        return Relation::ULT;
    case llvm::ICmpInst::Predicate::ICMP_SLT:
        return Relation::SLT;
    case llvm::ICmpInst::Predicate::ICMP_UGE:
        return Relation::UGE;
    case llvm::ICmpInst::Predicate::ICMP_SGE:
        return Relation::SGE;
    case llvm::ICmpInst::Predicate::ICMP_UGT:
        return Relation::UGT;
    case llvm::ICmpInst::Predicate::ICMP_SGT:
        return Relation::SGT;
    default:
#ifndef NDEBUG
        llvm::errs() << "Unhandled predicate in" << *icmp << "\n";
#endif
        abort();
    }
}

bool RelationsAnalyzer::processICMP(const ValueRelations &oldGraph,
                                    ValueRelations &newGraph,
                                    VRAssumeBool *assume) const {
    const llvm::ICmpInst *icmp = llvm::cast<llvm::ICmpInst>(assume->getValue());
    bool assumption = assume->getAssumption();

    V op1 = icmp->getOperand(0);
    V op2 = icmp->getOperand(1);

    Relation rel = ICMPToRel(icmp, assumption);

    if (oldGraph.hasConflictingRelation(op1, op2, rel))
        return false;

    newGraph.set(op1, rel, op2);
    return true;
}

bool RelationsAnalyzer::processPhi(ValueRelations &newGraph,
                                   VRAssumeBool *assume) const {
    const llvm::PHINode *phi = llvm::cast<llvm::PHINode>(assume->getValue());
    bool assumption = assume->getAssumption();

    const llvm::BasicBlock *assumedPred = nullptr;
    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        V result = phi->getIncomingValue(i);
        auto constResult = llvm::dyn_cast<llvm::ConstantInt>(result);
        if (!constResult ||
            (constResult && ((constResult->isOne() && assumption) ||
                             (constResult->isZero() && !assumption)))) {
            if (!assumedPred)
                assumedPred = phi->getIncomingBlock(i);
            else
                return true; // we found other viable incoming block
        }
    }
    assert(assumedPred);
    assert(assumedPred->size() > 1);
    const llvm::Instruction &lastBeforeTerminator =
            *std::prev(std::prev(assumedPred->end()));

    VRLocation &source = codeGraph.getVRLocation(&lastBeforeTerminator);
    bool result = newGraph.merge(source.relations);
    assert(result);
    return true;
}

// *********************** merge helpers **************************** //
Relations RelationsAnalyzer::getCommon(const VRLocation &location, V lt,
                                       Relations known, V rt) const {
    for (VREdge *predEdge : location.predecessors) {
        known &= predEdge->source->relations.between(lt, rt);
        if (!known.any())
            break;
    }
    return known;
}

void RelationsAnalyzer::checkRelatesInAll(VRLocation &location, V lt,
                                          Relations known, V rt,
                                          std::set<V> &setEqual) {
    if (lt == rt) // would add a bucket for every value, even if not related
        return;

    ValueRelations &newGraph = location.relations;

    Relations related = getCommon(location, lt, known, rt);
    if (!related.any())
        return;

    if (related.has(Relation::EQ))
        setEqual.emplace(rt);
    newGraph.set(lt, related, rt);
}

Relations RelationsAnalyzer::getCommonByPointedTo(
        V from, const std::vector<const ValueRelations *> &changeRelations,
        V val, Relations rels) {
    for (unsigned i = 1; i < changeRelations.size(); ++i) {
        assert(changeRelations[i]->hasLoad(from));
        Handle loaded = changeRelations[i]->getPointedTo(from);

        rels &= changeRelations[i]->between(loaded, val);
        if (!rels.any())
            break;
    }
    return rels;
}

Relations RelationsAnalyzer::getCommonByPointedTo(
        V from, const std::vector<const ValueRelations *> &changeRelations,
        V firstLoad, V prevVal) {
    Relations result = Relations().eq().addImplied();
    for (unsigned i = 1; i < changeRelations.size();
         ++i) { // zeroth relations are tree predecessor's
        Handle loaded = changeRelations[i]->getPointedTo(from);
        if (firstLoad)
            result &= changeRelations[i]->between(loaded, firstLoad);
        else
            result &= changeRelations[i]->between(loaded, prevVal);
        if (!result.any())
            break; // no common relations
    }
    return result;
}

std::pair<std::vector<const ValueRelations *>, V>
RelationsAnalyzer::getChangeRelations(V from, VRLocation &join) {
    if (!join.isJustLoopJoin() && !join.isJustBranchJoin())
        return {{}, nullptr};
    if (join.isJustBranchJoin()) {
        std::vector<const ValueRelations *> changeLocations;
        for (unsigned i = 0; i < join.predsSize(); ++i) {
            auto &relations = join.getPredLocation(i)->relations;
            if (!relations.hasLoad(from))
                return {};
            changeLocations.emplace_back(&relations);
        }
        return {changeLocations, nullptr};
    }
    assert(join.isJustLoopJoin());

    std::vector<const ValueRelations *> changeRelations = {
            &join.getTreePredecessor().relations};
    V firstLoad = nullptr;
    unsigned forks = 0;

    for (auto &inloopInst : structure.getInloopValues(join)) {
        VRLocation &targetLoc =
                *codeGraph.getVRLocation(inloopInst).getSuccLocation(0);

        if (auto load = llvm::dyn_cast<llvm::LoadInst>(inloopInst)) {
            if (load->getPointerOperand() == from && !firstLoad && !forks) {
                firstLoad = load;
            }
        }

        if (targetLoc.succsSize() > 1)
            ++forks;
        else if (targetLoc.isJustBranchJoin()) {
            assert(forks > 0);
            --forks;
        }

        if (mayOverwrite(inloopInst, from)) {
            if (!targetLoc.relations.hasLoad(from)) {
                return {{}, nullptr}; // no merge by load can happen here
            }
            changeRelations.emplace_back(&targetLoc.relations);
            ++forks; // will never get zeroed out now, no first load will be set
        }
    }
    return {changeRelations, firstLoad};
}

std::pair<RelationsAnalyzer::C, Relations>
RelationsAnalyzer::getBoundOnPointedToValue(
        const std::vector<const ValueRelations *> &changeRelations, V from,
        Relation rel) const {
    C bound = nullptr;
    Relations current = allRelations;

    for (const ValueRelations *graph : changeRelations) {
        if (!graph->hasLoad(from))
            return {nullptr, current};

        Handle pointedTo = graph->getPointedTo(from);
        auto valueRels = graph->getBound(pointedTo, rel);

        if (!valueRels.first)
            return {nullptr, current};

        if (!bound || ValueRelations::compare(bound, Relations::getStrict(rel),
                                              valueRels.first)) {
            bound = valueRels.first;
            current = Relations().set(Relations::getStrict(rel)).addImplied();
        }

        current &= valueRels.second;
        assert(current.any());
    }
    return {bound, current};
}

void RelationsAnalyzer::relateToFirstLoad(
        const std::vector<const ValueRelations *> &changeRelations, V from,
        ValueRelations &newGraph, Handle placeholder, V firstLoad) {
    Handle pointedTo = changeRelations[0]->getPointedTo(from);

    for (V prevVal : changeRelations[0]->getEqual(pointedTo)) {
        Relations common =
                getCommonByPointedTo(from, changeRelations, firstLoad, prevVal);
        if (common.any())
            newGraph.set(placeholder, common, prevVal);
    }
}

void RelationsAnalyzer::relateBounds(
        const std::vector<const ValueRelations *> &changeRelations, V from,
        ValueRelations &newGraph, Handle placeholder) {
    auto signedLowerBound =
            getBoundOnPointedToValue(changeRelations, from, Relation::SGE);
    auto unsignedLowerBound = getBoundOnPointedToValue(
            changeRelations, from,
            Relation::UGE); // TODO collect upper bound too

    if (signedLowerBound.first)
        newGraph.set(placeholder, signedLowerBound.second,
                     signedLowerBound.first);

    if (unsignedLowerBound.first)
        newGraph.set(placeholder, unsignedLowerBound.second,
                     unsignedLowerBound.first);
}

void RelationsAnalyzer::relateValues(
        const std::vector<const ValueRelations *> &changeRelations, V from,
        ValueRelations &newGraph, Handle placeholder) {
    const ValueRelations &predGraph = *changeRelations[0];
    Handle pointedTo = predGraph.getPointedTo(from);

    for (auto pair : predGraph.getRelated(pointedTo, comparative)) {
        Handle relatedH = pair.first;
        Relations relations = pair.second;

        assert(predGraph.are(pointedTo, relations, relatedH));

        if (relatedH == pointedTo)
            continue;

        for (V related : predGraph.getEqual(relatedH)) {
            Relations common = getCommonByPointedTo(from, changeRelations,
                                                    related, relations);
            if (common.any())
                newGraph.set(placeholder, common, related);
        }
    }
}

// **************************** merge ******************************* //
void RelationsAnalyzer::mergeRelations(VRLocation &location) {
    assert(location.predsSize() > 1);

    const ValueRelations &predGraph = location.getTreePredecessor().relations;

    std::set<V> setEqual;
    for (const auto &bucketVal : predGraph.getBucketToVals()) {
        for (const auto &related :
             predGraph.getRelated(bucketVal.first, restricted)) {
            for (V lt : bucketVal.second) {
                if (setEqual.find(lt) !=
                    setEqual.end()) // value has already been set equal to other
                    continue;
                for (V rt : predGraph.getEqual(related.first)) {
                    checkRelatesInAll(location, lt, related.second, rt,
                                      setEqual);
                }
            }
        }
    }

    ValueRelations &thisGraph = location.relations;

    // merge relations from tree predecessor only
    if (location.isJustLoopJoin()) {
        bool result = thisGraph.merge(predGraph, comparative);
        assert(result);
    }
}

void RelationsAnalyzer::mergeRelationsByPointedTo(VRLocation &loc) {
    ValueRelations &newGraph = loc.relations;
    ValueRelations &predGraph = loc.getTreePredecessor().relations;

    for (auto it = predGraph.begin_buckets(Relations().pt());
         it != predGraph.end_buckets(); ++it) {
        for (V from : predGraph.getEqual(it->from())) {
            Handle placeholder = newGraph.newPlaceholderBucket(from);

            std::vector<const ValueRelations *> changeLocations;
            V firstLoad;
            std::tie(changeLocations, firstLoad) =
                    getChangeRelations(from, loc);

            if (changeLocations.empty())
                continue;

            if (loc.isJustLoopJoin())
                relateToFirstLoad(changeLocations, from, newGraph, placeholder,
                                  firstLoad);
            relateBounds(changeLocations, from, newGraph, placeholder);
            relateValues(changeLocations, from, newGraph, placeholder);

            if (!newGraph.getEqual(placeholder).empty() ||
                newGraph.hasAnyRelation(placeholder))
                newGraph.setLoad(from, placeholder);
            else
                newGraph.erasePlaceholderBucket(placeholder);
        }
    }
}

// ***************************** edge ******************************* //
void RelationsAnalyzer::processInstruction(ValueRelations &graph, I inst) {
    switch (inst->getOpcode()) {
    case llvm::Instruction::Store:
        return storeGen(graph, llvm::dyn_cast<llvm::StoreInst>(inst));
    case llvm::Instruction::Load:
        return loadGen(graph, llvm::dyn_cast<llvm::LoadInst>(inst));
    case llvm::Instruction::GetElementPtr:
        return gepGen(graph, llvm::cast<llvm::GetElementPtrInst>(inst));
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt: // (S)ZExt should not change value
        return extGen(graph, llvm::dyn_cast<llvm::CastInst>(inst));
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::Mul:
        return opGen(graph, llvm::dyn_cast<llvm::BinaryOperator>(inst));
    case llvm::Instruction::SRem:
    case llvm::Instruction::URem:
        return remGen(graph, llvm::dyn_cast<llvm::BinaryOperator>(inst));
    default:
        if (auto cast = llvm::dyn_cast<llvm::CastInst>(inst)) {
            return castGen(graph, cast);
        }
    }
}

void RelationsAnalyzer::rememberValidated(const ValueRelations &prev,
                                          ValueRelations &graph, I inst) const {
    assert(&prev == &codeGraph.getVRLocation(inst).relations);

    for (auto it = prev.begin_buckets(Relations().pt());
         it != prev.end_buckets(); ++it) {
        for (V from : prev.getEqual(it->from())) {
            if (!mayOverwrite(inst, from)) {
                for (V to : prev.getEqual(it->to()))
                    graph.set(from, Relations::PT, to);
            }
        }
    }
}

bool RelationsAnalyzer::processAssumeBool(const ValueRelations &oldGraph,
                                          ValueRelations &newGraph,
                                          VRAssumeBool *assume) const {
    if (llvm::isa<llvm::ICmpInst>(assume->getValue()))
        return processICMP(oldGraph, newGraph, assume);
    if (llvm::isa<llvm::PHINode>(assume->getValue()))
        return processPhi(newGraph, assume);
    return false; // TODO; probably call
}

bool RelationsAnalyzer::processAssumeEqual(const ValueRelations &oldGraph,
                                           ValueRelations &newGraph,
                                           VRAssumeEqual *assume) const {
    V val1 = assume->getValue();
    V val2 = assume->getAssumption();
    if (oldGraph.hasConflictingRelation(val1, val2, Relation::EQ))
        return false;
    newGraph.setEqual(val1, val2);
    return true;
}

// ************************* topmost ******************************* //
void RelationsAnalyzer::processOperation(VRLocation *source, VRLocation *target,
                                         VROp *op) {
    if (!target)
        return;
    assert(source && target && op);

    ValueRelations &newGraph = target->relations;

    if (op->isInstruction()) {
        newGraph.merge(source->relations, comparative);
        I inst = static_cast<VRInstruction *>(op)->getInstruction();
        rememberValidated(source->relations, newGraph, inst);
        processInstruction(newGraph, inst);

    } else if (op->isAssume()) {
        newGraph.merge(source->relations, Relations().pt());
        bool shouldMerge;
        if (op->isAssumeBool())
            shouldMerge = processAssumeBool(source->relations, newGraph,
                                            static_cast<VRAssumeBool *>(op));
        else // isAssumeEqual
            shouldMerge = processAssumeEqual(source->relations, newGraph,
                                             static_cast<VRAssumeEqual *>(op));
        if (shouldMerge)
            newGraph.merge(source->relations, comparative);

    } else { // else op is noop
        newGraph.merge(source->relations, allRelations);
    }
}

bool RelationsAnalyzer::passFunction(const llvm::Function &function,
                                     bool print) {
    bool changed = false;

    for (auto it = codeGraph.lazy_dfs_begin(function);
         it != codeGraph.lazy_dfs_end(); ++it) {
        VRLocation &location = *it;
        bool cond = location.id == 91;
        if (print && cond) {
            std::cerr << "LOCATION " << location.id << std::endl;
            for (VREdge *predEdge : location.predecessors) {
                std::cerr << predEdge->op->toStr() << std::endl;
            }
        }
        if (print && cond) {
            for (unsigned i = 0; i < location.predsSize(); ++i) {
                std::cerr << "pred" << i << "\n";
                std::cerr << location.getPredLocation(i)->relations << "\n";
            }
            std::cerr << "before\n" << location.relations << "\n";
        }

        if (location.predsSize() > 1) {
            mergeRelations(location);
            mergeRelationsByPointedTo(location);
        } else if (location.predsSize() == 1) {
            VREdge *edge = location.getPredEdge(0);
            processOperation(edge->source, edge->target, edge->op.get());
        } // else no predecessors => nothing to be passed

        bool locationChanged = location.relations.unsetChanged();
        if (print && cond /*&& locationChanged*/) {
            std::cerr << "after\n" << location.relations;
            return false;
        }
        changed |= locationChanged;
    }
    return changed;
}

unsigned RelationsAnalyzer::analyze(unsigned maxPass) {
    unsigned maxExecutedPass = 0;

    for (auto &function : module) {
        if (function.isDeclaration())
            continue;

        bool changed = true;
        unsigned passNum = 0;
        while (changed && passNum < maxPass) {
            changed = passFunction(function, false); // passNum+1==maxPass);
            ++passNum;
        }

        maxExecutedPass = std::max(maxExecutedPass, passNum);
    }

    return maxExecutedPass;
}

} // namespace vr
} // namespace dg
