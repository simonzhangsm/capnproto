// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "node-translator.h"
#include "parser.h"      // only for generateGroupId()
#include <kj/debug.h>
#include <kj/arena.h>
#include <set>
#include <map>
#include <limits>

namespace capnp {
namespace compiler {

class NodeTranslator::StructLayout {
  // Massive, disgusting class which implements the layout algorithm, which decides the offset
  // for each field.

public:
  template <typename UIntType>
  struct HoleSet {
    inline HoleSet(): holes{0, 0, 0, 0, 0, 0} {}

    // Represents a set of "holes" within a segment of allocated space, up to one hole of each
    // power-of-two size between 1 bit and 32 bits.
    //
    // The amount of "used" space in a struct's data segment can always be represented as a
    // combination of a word count and a HoleSet.  The HoleSet represents the space lost to
    // "padding".
    //
    // There can never be more than one hole of any particular size.  Why is this?  Well, consider
    // that every data field has a power-of-two size, every field must be aligned to a multiple of
    // its size, and the maximum size of a single field is 64 bits.  If we need to add a new field
    // of N bits, there are two possibilities:
    // 1. A hole of size N or larger exists.  In this case, we find the smallest hole that is at
    //    least N bits.  Let's say that that hole has size M.  We allocate the first N bits of the
    //    hole to the new field.  The remaining M - N bits become a series of holes of sizes N*2,
    //    N*4, ..., M / 2.  We know no holes of these sizes existed before because we chose M to be
    //    the smallest available hole larger than N.  So, there is still no more than one hole of
    //    each size, and no hole larger than any hole that existed previously.
    // 2. No hole equal or larger N exists.  In that case we extend the data section's size by one
    //    word, creating a new 64-bit hole at the end.  We then allocate N bits from it, creating
    //    a series of holes between N and 64 bits, as described in point (1).  Thus, again, there
    //    is still at most one hole of each size, and the largest hole is 32 bits.

    UIntType holes[6];
    // The offset of each hole as a multiple of its size.  A value of zero indicates that no hole
    // exists.  Notice that it is impossible for any actual hole to have an offset of zero, because
    // the first field allocated is always placed at the very beginning of the section.  So either
    // the section has a size of zero (in which case there are no holes), or offset zero is
    // already allocated and therefore cannot be a hole.

    kj::Maybe<UIntType> tryAllocate(UIntType lgSize) {
      // Try to find space for a field of size lgSize^2 within the set of holes.  If found,
      // remove it from the holes, and return its offset (as a multiple of its size).  If there
      // is no such space, returns zero (no hole can be at offset zero, as explained above).

      if (lgSize >= KJ_ARRAY_SIZE(holes)) {
        return nullptr;
      } else if (holes[lgSize] != 0) {
        UIntType result = holes[lgSize];
        holes[lgSize] = 0;
        return result;
      } else {
        KJ_IF_MAYBE(next, tryAllocate(lgSize + 1)) {
          UIntType result = *next * 2;
          holes[lgSize] = result + 1;
          return result;
        } else {
          return nullptr;
        }
      }
    }

    uint assertHoleAndAllocate(UIntType lgSize) {
      KJ_ASSERT(holes[lgSize] != 0);
      uint result = holes[lgSize];
      holes[lgSize] = 0;
      return result;
    }

    void addHolesAtEnd(UIntType lgSize, UIntType offset,
                       UIntType limitLgSize = KJ_ARRAY_SIZE(holes)) {
      // Add new holes of progressively larger sizes in the range [lgSize, limitLgSize) starting
      // from the given offset.  The idea is that you just allocated an lgSize-sized field from
      // an limitLgSize-sized space, such as a newly-added word on the end of the data segment.

      KJ_DREQUIRE(limitLgSize <= KJ_ARRAY_SIZE(holes));

      while (lgSize < limitLgSize) {
        KJ_DREQUIRE(holes[lgSize] == 0);
        KJ_DREQUIRE(offset % 2 == 1);
        holes[lgSize] = offset;
        ++lgSize;
        offset = (offset + 1) / 2;
      }
    }

    bool tryExpand(UIntType oldLgSize, uint oldOffset, uint expansionFactor) {
      // Try to expand the value at the given location by combining it with subsequent holes, so
      // as to expand the location to be 2^expansionFactor times the size that it started as.
      // (In other words, the new lgSize is oldLgSize + expansionFactor.)

      if (expansionFactor == 0) {
        // No expansion requested.
        return true;
      }
      if (holes[oldLgSize] != oldOffset + 1) {
        // The space immediately after the location is not a hole.
        return false;
      }

      // We can expand the location by one factor by combining it with a hole.  Try to further
      // expand from there to the number of factors requested.
      if (tryExpand(oldLgSize + 1, oldOffset >> 1, expansionFactor - 1)) {
        // Success.  Consume the hole.
        holes[oldLgSize] = 0;
        return true;
      } else {
        return false;
      }
    }

    kj::Maybe<uint> smallestAtLeast(uint size) {
      // Return the size of the smallest hole that is equal to or larger than the given size.

      for (uint i = size; i < KJ_ARRAY_SIZE(holes); i++) {
        if (holes[i] != 0) {
          return i;
        }
      }
      return nullptr;
    }

    uint getFirstWordUsed() {
      // Computes the lg of the amount of space used in the first word of the section.

      // If there is a 32-bit hole with a 32-bit offset, no more than the first 32 bits are used.
      // If no more than the first 32 bits are used, and there is a 16-bit hole with a 16-bit
      // offset, then no more than the first 16 bits are used.  And so on.
      for (uint i = KJ_ARRAY_SIZE(holes); i > 0; i--) {
        if (holes[i - 1] != 1) {
          return i;
        }
      }
      return 0;
    }
  };

  struct StructOrGroup {
    // Abstract interface for scopes in which fields can be added.

    virtual void addVoid() = 0;
    virtual uint addData(uint lgSize) = 0;
    virtual uint addPointer() = 0;
    virtual bool tryExpandData(uint oldLgSize, uint oldOffset, uint expansionFactor) = 0;
    // Try to expand the given previously-allocated space by 2^expansionFactor.  Succeeds --
    // returning true -- if the following space happens to be empty, making this expansion possible.
    // Otherwise, returns false.
  };

  struct Top: public StructOrGroup {
    uint dataWordCount = 0;
    uint pointerCount = 0;
    // Size of the struct so far.

    HoleSet<uint> holes;

    void addVoid() override {}

    uint addData(uint lgSize) override {
      KJ_IF_MAYBE(hole, holes.tryAllocate(lgSize)) {
        return *hole;
      } else {
        uint offset = dataWordCount++ << (6 - lgSize);
        holes.addHolesAtEnd(lgSize, offset + 1);
        return offset;
      }
    }

    uint addPointer() override {
      return pointerCount++;
    }

    bool tryExpandData(uint oldLgSize, uint oldOffset, uint expansionFactor) override {
      return holes.tryExpand(oldLgSize, oldOffset, expansionFactor);
    }

    Top() = default;
    KJ_DISALLOW_COPY(Top);
  };

  struct Union {
    struct DataLocation {
      uint lgSize;
      uint offset;

      bool tryExpandTo(Union& u, uint newLgSize) {
        if (newLgSize <= lgSize) {
          return true;
        } else if (u.parent.tryExpandData(lgSize, offset, newLgSize - lgSize)) {
          offset >>= (newLgSize - lgSize);
          lgSize = newLgSize;
          return true;
        } else {
          return false;
        }
      }
    };

    StructOrGroup& parent;
    uint groupCount = 0;
    kj::Maybe<uint> discriminantOffset;
    kj::Vector<DataLocation> dataLocations;
    kj::Vector<uint> pointerLocations;

    inline Union(StructOrGroup& parent): parent(parent) {}
    KJ_DISALLOW_COPY(Union);

    uint addNewDataLocation(uint lgSize) {
      // Add a whole new data location to the union with the given size.

      uint offset = parent.addData(lgSize);
      dataLocations.add(DataLocation { lgSize, offset });
      return offset;
    }

    uint addNewPointerLocation() {
      // Add a whole new pointer location to the union with the given size.

      return pointerLocations.add(parent.addPointer());
    }

    void newGroupAddingFirstMember() {
      if (++groupCount == 2) {
        addDiscriminant();
      }
    }

    bool addDiscriminant() {
      if (discriminantOffset == nullptr) {
        discriminantOffset = parent.addData(4);  // 2^4 = 16 bits
        return true;
      } else {
        return false;
      }
    }
  };

  struct Group: public StructOrGroup {
  public:
    class DataLocationUsage {
    public:
      DataLocationUsage(): isUsed(false) {}
      explicit DataLocationUsage(uint lgSize): isUsed(true), lgSizeUsed(lgSize) {}

      kj::Maybe<uint> smallestHoleAtLeast(Union::DataLocation& location, uint lgSize) {
        // Find the smallest single hole that is at least the given size.  This is used to find the
        // optimal place to allocate each field -- it is placed in the smallest slot where it fits,
        // to reduce fragmentation.  Returns the size of the hole, if found.

        if (!isUsed) {
          // The location is effectively one big hole.
          if (lgSize <= location.lgSize) {
            return location.lgSize;
          } else {
            return nullptr;
          }
        } else if (lgSize >= lgSizeUsed) {
          // Requested size is at least our current usage, so clearly won't fit in any current
          // holes, but if the location's size is larger than what we're using, we'd be able to
          // expand.
          if (lgSize < location.lgSize) {
            return lgSize;
          } else {
            return nullptr;
          }
        } else KJ_IF_MAYBE(result, holes.smallestAtLeast(lgSize)) {
          // There's a hole.
          return *result;
        } else {
          // The requested size is smaller than what we're already using, but there are no holes
          // available.  If we could double our size, then we could allocate in the new space.

          if (lgSizeUsed < location.lgSize) {
            // We effectively create a new hole the same size as the current usage.
            return lgSizeUsed;
          } else {
            return nullptr;
          }
        }
      }

      uint allocateFromHole(Group& group, Union::DataLocation& location, uint lgSize) {
        // Allocate the given space from an existing hole, given smallestHoleAtLeast() already
        // returned non-null indicating such a hole exists.

        uint result;

        if (!isUsed) {
          // The location is totally unused, so just allocate from the beginning.
          KJ_DASSERT(lgSize <= location.lgSize, "Did smallestHoleAtLeast() really find a hole?");
          result = 0;
          isUsed = true;
          lgSizeUsed = lgSize;
        } else if (lgSize >= lgSizeUsed) {
          // Requested size is at least our current usage, so clearly won't fit in any holes.
          // We must expand to double the requested size, and return the second half.
          KJ_DASSERT(lgSize < location.lgSize, "Did smallestHoleAtLeast() really find a hole?");
          holes.addHolesAtEnd(lgSizeUsed, 1, lgSize);
          lgSizeUsed = lgSize + 1;
          result = 1;
        } else KJ_IF_MAYBE(hole, holes.tryAllocate(lgSize)) {
          // Found a hole.
          result = *hole;
        } else {
          // The requested size is smaller than what we're using so far, but didn't fit in a
          // hole.  We should double our "used" size, then allocate from the new space.
          KJ_DASSERT(lgSizeUsed < location.lgSize,
                     "Did smallestHoleAtLeast() really find a hole?");
          result = 1 << (lgSizeUsed - lgSize);
          holes.addHolesAtEnd(lgSize, result + 1, lgSizeUsed);
          lgSizeUsed += 1;
        }

        // Adjust the offset according to the location's offset before returning.
        uint locationOffset = location.offset << (location.lgSize - lgSize);
        return locationOffset + result;
      }

      kj::Maybe<uint> tryAllocateByExpanding(
          Group& group, Union::DataLocation& location, uint lgSize) {
        // Attempt to allocate the given size by requesting that the parent union expand this
        // location to fit.  This is used if smallestHoleAtLeast() already determined that there
        // are no holes that would fit, so we don't bother checking that.

        if (!isUsed) {
          if (location.tryExpandTo(group.parent, lgSize)) {
            isUsed = true;
            lgSizeUsed = lgSize;
            return location.offset << (location.lgSize - lgSize);
          } else {
            return nullptr;
          }
        } else {
          uint newSize = kj::max(lgSizeUsed, lgSize) + 1;
          if (tryExpandUsage(group, location, newSize)) {
            uint result = KJ_ASSERT_NONNULL(holes.tryAllocate(lgSize));
            uint locationOffset = location.offset << (location.lgSize - lgSize);
            return locationOffset + result;
          } else {
            return nullptr;
          }
        }
      }

      bool tryExpand(Group& group, Union::DataLocation& location,
                     uint oldLgSize, uint oldOffset, uint expansionFactor) {
        if (oldOffset == 0 && lgSizeUsed == oldLgSize) {
          // This location contains exactly the requested data, so just expand the whole thing.
          return tryExpandUsage(group, location, oldLgSize + expansionFactor);
        } else {
          // This location contains the requested data plus other stuff.  Therefore the data cannot
          // possibly expand past the end of the space we've already marked used without either
          // overlapping with something else or breaking alignment rules.  We only have to combine
          // it with holes.
          return holes.tryExpand(oldLgSize, oldOffset, expansionFactor);
        }
      }

    private:
      bool isUsed;
      // Whether or not this location has been used at all by the group.

      uint8_t lgSizeUsed;
      // Amount of space from the location which is "used".  This is the minimum size needed to
      // cover all allocated space.  Only meaningful if `isUsed` is true.

      HoleSet<uint8_t> holes;
      // Indicates holes present in the space designated by `lgSizeUsed`.  The offsets in this
      // HoleSet are relative to the beginning of this particular data location, not the beginning
      // of the struct.

      bool tryExpandUsage(Group& group, Union::DataLocation& location, uint desiredUsage) {
        if (desiredUsage > location.lgSize) {
          // Need to expand the underlying slot.
          if (!location.tryExpandTo(group.parent, desiredUsage)) {
            return false;
          }
        }

        // Underlying slot is big enough, so expand our size and update holes.
        holes.addHolesAtEnd(lgSizeUsed, 1, desiredUsage);
        lgSizeUsed = desiredUsage;
        return true;
      }
    };

    Union& parent;

    kj::Vector<DataLocationUsage> parentDataLocationUsage;
    // Vector corresponding to the parent union's `dataLocations`, indicating how much of each
    // location has already been allocated.

    uint parentPointerLocationUsage = 0;
    // Number of parent's pointer locations that have been used by this group.

    bool hasMembers = false;

    inline Group(Union& parent): parent(parent) {}
    KJ_DISALLOW_COPY(Group);

    void addVoid() override {
      if (!hasMembers) {
        hasMembers = true;
        parent.newGroupAddingFirstMember();
      }
    }

    uint addData(uint lgSize) override {
      addVoid();

      uint bestSize = std::numeric_limits<uint>::max();
      kj::Maybe<uint> bestLocation = nullptr;

      for (uint i = 0; i < parent.dataLocations.size(); i++) {
        // If we haven't seen this DataLocation yet, add a corresponding DataLocationUsage.
        if (parentDataLocationUsage.size() == i) {
          parentDataLocationUsage.add();
        }

        auto& usage = parentDataLocationUsage[i];
        KJ_IF_MAYBE(hole, usage.smallestHoleAtLeast(parent.dataLocations[i], lgSize)) {
          if (*hole < bestSize) {
            bestSize = *hole;
            bestLocation = i;
          }
        }
      }

      KJ_IF_MAYBE(best, bestLocation) {
        return parentDataLocationUsage[*best].allocateFromHole(
            *this, parent.dataLocations[*best], lgSize);
      }

      // There are no holes at all in the union big enough to fit this field.  Go back through all
      // of the locations and attempt to expand them to fit.
      for (uint i = 0; i < parent.dataLocations.size(); i++) {
        KJ_IF_MAYBE(result, parentDataLocationUsage[i].tryAllocateByExpanding(
            *this, parent.dataLocations[i], lgSize)) {
          return *result;
        }
      }

      // Couldn't find any space in the existing locations, so add a new one.
      uint result = parent.addNewDataLocation(lgSize);
      parentDataLocationUsage.add(lgSize);
      return result;
    }

    uint addPointer() override {
      addVoid();

      if (parentPointerLocationUsage < parent.pointerLocations.size()) {
        return parent.pointerLocations[parentPointerLocationUsage++];
      } else {
        parentPointerLocationUsage++;
        return parent.addNewPointerLocation();
      }
    }

    bool tryExpandData(uint oldLgSize, uint oldOffset, uint expansionFactor) override {
      if (oldLgSize + expansionFactor > 6 ||
          (oldOffset & ((1 << expansionFactor) - 1)) != 0) {
        // Expansion is not possible because the new size is too large or the offset is not
        // properly-aligned.
      }

      for (uint i = 0; i < parentDataLocationUsage.size(); i++) {
        auto& location = parent.dataLocations[i];
        if (location.lgSize >= oldLgSize &&
            oldOffset >> (location.lgSize - oldLgSize) == location.offset) {
          // The location we're trying to expand is a subset of this data location.
          auto& usage = parentDataLocationUsage[i];

          // Adjust the offset to be only within this location.
          uint localOldOffset = oldOffset - (location.offset << (location.lgSize - oldLgSize));

          // Try to expand.
          return usage.tryExpand(*this, location, oldLgSize, localOldOffset, expansionFactor);
        }
      }

      KJ_FAIL_ASSERT("Tried to expand field that was never allocated.");
      return false;
    }
  };

  Top& getTop() { return top; }

private:
  Top top;
};

// =======================================================================================

NodeTranslator::NodeTranslator(
    const Resolver& resolver, const ErrorReporter& errorReporter,
    const Declaration::Reader& decl, Orphan<schema2::Node> wipNodeParam,
    bool compileAnnotations)
    : resolver(resolver), errorReporter(errorReporter),
      compileAnnotations(compileAnnotations), wipNode(kj::mv(wipNodeParam)) {
  compileNode(decl, wipNode.get());
}

NodeTranslator::NodeSet NodeTranslator::getBootstrapNode() {
  return NodeSet {
    wipNode.getReader(),
    KJ_MAP(groups, g) { return g.getReader(); }
  };
}

NodeTranslator::NodeSet NodeTranslator::finish() {
  // Careful about iteration here:  compileFinalValue() may actually add more elements to
  // `unfinishedValues`, invalidating iterators in the process.
  for (size_t i = 0; i < unfinishedValues.size(); i++) {
    auto& value = unfinishedValues[i];
    compileValue(value.source, value.type, value.target, false);
  }

  return getBootstrapNode();
}

class NodeTranslator::DuplicateNameDetector {
public:
  inline explicit DuplicateNameDetector(const ErrorReporter& errorReporter)
      : errorReporter(errorReporter) {}
  void check(List<Declaration>::Reader nestedDecls, Declaration::Body::Which parentKind);

private:
  const ErrorReporter& errorReporter;
  std::map<kj::StringPtr, LocatedText::Reader> names;
};

void NodeTranslator::compileNode(Declaration::Reader decl, schema2::Node::Builder builder) {
  DuplicateNameDetector(errorReporter)
      .check(decl.getNestedDecls(), decl.getBody().which());

  kj::StringPtr targetsFlagName;

  switch (decl.getBody().which()) {
    case Declaration::Body::FILE_DECL:
      targetsFlagName = "targetsFile";
      break;
    case Declaration::Body::CONST_DECL:
      compileConst(decl.getBody().getConstDecl(), builder.initConst());
      targetsFlagName = "targetsConst";
      break;
    case Declaration::Body::ANNOTATION_DECL:
      compileAnnotation(decl.getBody().getAnnotationDecl(), builder.initAnnotation());
      targetsFlagName = "targetsAnnotation";
      break;
    case Declaration::Body::ENUM_DECL:
      compileEnum(decl.getBody().getEnumDecl(), decl.getNestedDecls(), builder);
      targetsFlagName = "targetsEnum";
      break;
    case Declaration::Body::STRUCT_DECL:
      compileStruct(decl.getBody().getStructDecl(), decl.getNestedDecls(), builder);
      targetsFlagName = "targetsStruct";
      break;
    case Declaration::Body::INTERFACE_DECL:
      compileInterface(decl.getBody().getInterfaceDecl(), decl.getNestedDecls(), builder);
      targetsFlagName = "targetsInterface";
      break;

    default:
      KJ_FAIL_REQUIRE("This Declaration is not a node.");
      break;
  }

  builder.adoptAnnotations(compileAnnotationApplications(decl.getAnnotations(), targetsFlagName));
}

void NodeTranslator::DuplicateNameDetector::check(
    List<Declaration>::Reader nestedDecls, Declaration::Body::Which parentKind) {
  for (auto decl: nestedDecls) {
    {
      auto name = decl.getName();
      auto nameText = name.getValue();
      auto insertResult = names.insert(std::make_pair(nameText, name));
      if (!insertResult.second) {
        if (nameText.size() == 0 && decl.getBody().which() == Declaration::Body::UNION_DECL) {
          errorReporter.addErrorOn(
              name, kj::str("An unnamed union is already defined in this scope."));
          errorReporter.addErrorOn(
              insertResult.first->second, kj::str("Previously defined here."));
        } else {
          errorReporter.addErrorOn(
              name, kj::str("'", nameText, "' is already defined in this scope."));
          errorReporter.addErrorOn(
              insertResult.first->second, kj::str("'", nameText, "' previously defined here."));
        }
      }
    }

    switch (decl.getBody().which()) {
      case Declaration::Body::USING_DECL:
      case Declaration::Body::CONST_DECL:
      case Declaration::Body::ENUM_DECL:
      case Declaration::Body::STRUCT_DECL:
      case Declaration::Body::INTERFACE_DECL:
      case Declaration::Body::ANNOTATION_DECL:
        switch (parentKind) {
          case Declaration::Body::FILE_DECL:
          case Declaration::Body::STRUCT_DECL:
          case Declaration::Body::INTERFACE_DECL:
            // OK.
            break;
          default:
            errorReporter.addErrorOn(decl, "This kind of declaration doesn't belong here.");
            break;
        }
        break;

      case Declaration::Body::ENUMERANT_DECL:
        if (parentKind != Declaration::Body::ENUM_DECL) {
          errorReporter.addErrorOn(decl, "Enumerants can only appear in enums.");
        }
        break;
      case Declaration::Body::METHOD_DECL:
        if (parentKind != Declaration::Body::INTERFACE_DECL) {
          errorReporter.addErrorOn(decl, "Methods can only appear in interfaces.");
        }
        break;
      case Declaration::Body::FIELD_DECL:
      case Declaration::Body::UNION_DECL:
      case Declaration::Body::GROUP_DECL:
        switch (parentKind) {
          case Declaration::Body::STRUCT_DECL:
          case Declaration::Body::UNION_DECL:
          case Declaration::Body::GROUP_DECL:
            // OK.
            break;
          default:
            errorReporter.addErrorOn(decl, "This declaration can only appear in structs.");
            break;
        }

        // Struct members may have nested decls.  We need to check those here, because no one else
        // is going to do it.
        if (decl.getName().getValue().size() == 0) {
          // Unnamed union.  Check members as if they are in the same scope.
          check(decl.getNestedDecls(), decl.getBody().which());
        } else {
          // Children are in their own scope.
          DuplicateNameDetector(errorReporter)
              .check(decl.getNestedDecls(), decl.getBody().which());
        }

        break;

      default:
        errorReporter.addErrorOn(decl, "This kind of declaration doesn't belong here.");
        break;
    }
  }
}

void NodeTranslator::disallowNested(List<Declaration>::Reader nestedDecls) {
  for (auto decl: nestedDecls) {
    errorReporter.addErrorOn(decl, "Nested declaration not allowed here.");
  }
}

void NodeTranslator::compileConst(Declaration::Const::Reader decl,
                                  schema2::Node::Const::Builder builder) {
  auto typeBuilder = builder.initType();
  if (compileType(decl.getType(), typeBuilder)) {
    compileBootstrapValue(decl.getValue(), typeBuilder.asReader(), builder.initValue());
  }
}

void NodeTranslator::compileAnnotation(Declaration::Annotation::Reader decl,
                                       schema2::Node::Annotation::Builder builder) {
  compileType(decl.getType(), builder.initType());

#warning "temporary hack for schema transition"
  builder.setTargetsFile(true);

#if 0
  // Dynamically copy over the values of all of the "targets" members.
  DynamicStruct::Reader src = decl;
  DynamicStruct::Builder dst = builder;
  for (auto srcMember: src.getSchema().getMembers()) {
    kj::StringPtr memberName = srcMember.getProto().getName();
    if (memberName.startsWith("targets")) {
      auto dstMember = dst.getSchema().getMemberByName(memberName);
      dst.set(dstMember, src.get(srcMember));
    }
  }
#endif
}

class NodeTranslator::DuplicateOrdinalDetector {
public:
  DuplicateOrdinalDetector(const ErrorReporter& errorReporter): errorReporter(errorReporter) {}

  void check(LocatedInteger::Reader ordinal) {
    if (ordinal.getValue() < expectedOrdinal) {
      errorReporter.addErrorOn(ordinal, "Duplicate ordinal number.");
      KJ_IF_MAYBE(last, lastOrdinalLocation) {
        errorReporter.addErrorOn(
            *last, kj::str("Ordinal @", last->getValue(), " originally used here."));
        // Don't report original again.
        lastOrdinalLocation = nullptr;
      }
    } else if (ordinal.getValue() > expectedOrdinal) {
      errorReporter.addErrorOn(ordinal,
          kj::str("Skipped ordinal @", expectedOrdinal, ".  Ordinals must be sequential with no "
                  "holes."));
      expectedOrdinal = ordinal.getValue() + 1;
    } else {
      ++expectedOrdinal;
      lastOrdinalLocation = ordinal;
    }
  }

private:
  const ErrorReporter& errorReporter;
  uint expectedOrdinal = 0;
  kj::Maybe<LocatedInteger::Reader> lastOrdinalLocation;
};

void NodeTranslator::compileEnum(Declaration::Enum::Reader decl,
                                 List<Declaration>::Reader members,
                                 schema2::Node::Builder builder) {
  // maps ordinal -> (code order, declaration)
  std::multimap<uint, std::pair<uint, Declaration::Reader>> enumerants;

  uint codeOrder = 0;
  for (auto member: members) {
    if (member.getBody().which() == Declaration::Body::ENUMERANT_DECL) {
      enumerants.insert(
          std::make_pair(member.getId().getOrdinal().getValue(),
                         std::make_pair(codeOrder++, member)));
    }
  }

  auto list = builder.initEnum(enumerants.size());
  uint i = 0;
  DuplicateOrdinalDetector dupDetector(errorReporter);

  for (auto& entry: enumerants) {
    uint codeOrder = entry.second.first;
    Declaration::Reader enumerantDecl = entry.second.second;

    dupDetector.check(enumerantDecl.getId().getOrdinal());

    auto enumerantBuilder = list[i++];
    enumerantBuilder.setName(enumerantDecl.getName().getValue());
    enumerantBuilder.setCodeOrder(codeOrder);
    enumerantBuilder.adoptAnnotations(compileAnnotationApplications(
        enumerantDecl.getAnnotations(), "targetsEnumerant"));
  }
}

// -------------------------------------------------------------------

class NodeTranslator::StructTranslator {
public:
  explicit StructTranslator(NodeTranslator& translator)
      : translator(translator), errorReporter(translator.errorReporter) {}
  KJ_DISALLOW_COPY(StructTranslator);

  void translate(Declaration::Struct::Reader decl, List<Declaration>::Reader members,
                 schema2::Node::Builder builder) {
    auto structBuilder = builder.initStruct();

    // Build the member-info-by-ordinal map.
    MemberInfo root(builder);
    traverseTopOrGroup(members, root, layout.getTop());

    // Go through each member in ordinal order, building each member schema.
    DuplicateOrdinalDetector dupDetector(errorReporter);
    for (auto& entry: membersByOrdinal) {
      MemberInfo& member = *entry.second;

      if (member.decl.getId().which() == Declaration::Id::ORDINAL) {
        dupDetector.check(member.decl.getId().getOrdinal());
      }

      schema2::Field::Builder fieldBuilder = member.getSchema();
      fieldBuilder.getOrdinal().setExplicit(entry.first);

      switch (member.decl.getBody().which()) {
        case Declaration::Body::FIELD_DECL: {
          auto fieldReader = member.decl.getBody().getFieldDecl();
          auto regularField = fieldBuilder.initRegular();
          auto typeBuilder = regularField.initType();
          if (translator.compileType(fieldReader.getType(), typeBuilder)) {
            switch (fieldReader.getDefaultValue().which()) {
              case Declaration::Field::DefaultValue::VALUE:
                translator.compileBootstrapValue(fieldReader.getDefaultValue().getValue(),
                                                 typeBuilder, regularField.initDefaultValue());
                break;
              case Declaration::Field::DefaultValue::NONE:
                translator.compileDefaultDefaultValue(typeBuilder, regularField.initDefaultValue());
                break;
            }
          } else {
            translator.compileDefaultDefaultValue(typeBuilder, regularField.initDefaultValue());
          }

          int lgSize = -1;
          switch (typeBuilder.which()) {
            case schema2::Type::VOID: lgSize = -1; break;
            case schema2::Type::BOOL: lgSize = 0; break;
            case schema2::Type::INT8: lgSize = 3; break;
            case schema2::Type::INT16: lgSize = 4; break;
            case schema2::Type::INT32: lgSize = 5; break;
            case schema2::Type::INT64: lgSize = 6; break;
            case schema2::Type::UINT8: lgSize = 3; break;
            case schema2::Type::UINT16: lgSize = 4; break;
            case schema2::Type::UINT32: lgSize = 5; break;
            case schema2::Type::UINT64: lgSize = 6; break;
            case schema2::Type::FLOAT32: lgSize = 5; break;
            case schema2::Type::FLOAT64: lgSize = 6; break;

            case schema2::Type::TEXT: lgSize = -2; break;
            case schema2::Type::DATA: lgSize = -2; break;
            case schema2::Type::LIST: lgSize = -2; break;
            case schema2::Type::ENUM: lgSize = 4; break;
            case schema2::Type::STRUCT: lgSize = -2; break;
            case schema2::Type::INTERFACE: lgSize = -2; break;
            case schema2::Type::OBJECT: lgSize = -2; break;
          }

          if (lgSize == -2) {
            // pointer
            regularField.setOffset(member.fieldScope->addPointer());
          } else if (lgSize == -1) {
            // void
            member.fieldScope->addVoid();
            regularField.setOffset(0);
          } else {
            regularField.setOffset(member.fieldScope->addData(lgSize));
          }
          break;
        }

        case Declaration::Body::UNION_DECL:
          if (!member.unionScope->addDiscriminant()) {
            errorReporter.addErrorOn(member.decl.getId().getOrdinal(),
                "Union ordinal, if specified, must be greater than no more than one of its "
                "member ordinals (i.e. there can only be one field retroactively unionized).");
          }
          break;

        case Declaration::Body::GROUP_DECL:
          KJ_FAIL_ASSERT("Groups don't have ordinals.");
          break;

        default:
          KJ_FAIL_ASSERT("Unexpected member type.");
          break;
      }
    }

    // OK, we should have built all the members.  Now go through and make sure the discriminant
    // offsets have been copied over to the schemas and annotations have been applied.
    root.finishGroup();
    for (auto member: allMembers) {
      kj::StringPtr targetsFlagName;
      switch (member->decl.getBody().which()) {
        case Declaration::Body::FIELD_DECL:
          targetsFlagName = "targetsField";
          break;

        case Declaration::Body::UNION_DECL:
          member->finishGroup();
          targetsFlagName = "targetsUnion";
          break;

        case Declaration::Body::GROUP_DECL:
          member->finishGroup();
          targetsFlagName = "targetsGroup";
          break;

        default:
          KJ_FAIL_ASSERT("Unexpected member type.");
          break;
      }

      builder.adoptAnnotations(translator.compileAnnotationApplications(
          member->decl.getAnnotations(), targetsFlagName));
    }

    // And fill in the sizes.
    structBuilder.setDataSectionWordSize(layout.getTop().dataWordCount);
    structBuilder.setPointerSectionSize(layout.getTop().pointerCount);
    structBuilder.setPreferredListEncoding(schema2::ElementSize::INLINE_COMPOSITE);

    if (layout.getTop().pointerCount == 0) {
      if (layout.getTop().dataWordCount == 0) {
        structBuilder.setPreferredListEncoding(schema2::ElementSize::EMPTY);
      } else if (layout.getTop().dataWordCount == 1) {
        switch (layout.getTop().holes.getFirstWordUsed()) {
          case 0: structBuilder.setPreferredListEncoding(schema2::ElementSize::BIT); break;
          case 1:
          case 2:
          case 3: structBuilder.setPreferredListEncoding(schema2::ElementSize::BYTE); break;
          case 4: structBuilder.setPreferredListEncoding(schema2::ElementSize::TWO_BYTES); break;
          case 5: structBuilder.setPreferredListEncoding(schema2::ElementSize::FOUR_BYTES); break;
          case 6: structBuilder.setPreferredListEncoding(schema2::ElementSize::EIGHT_BYTES); break;
          default: KJ_FAIL_ASSERT("Expected 0, 1, 2, 3, 4, 5, or 6."); break;
        }
      }
    } else if (layout.getTop().pointerCount == 1 &&
               layout.getTop().dataWordCount == 0) {
      structBuilder.setPreferredListEncoding(schema2::ElementSize::POINTER);
    }

    for (auto& group: translator.groups) {
      auto groupBuilder = group.get().getStruct();
      groupBuilder.setDataSectionWordSize(structBuilder.getDataSectionWordSize());
      groupBuilder.setPointerSectionSize(structBuilder.getPointerSectionSize());
      groupBuilder.setPreferredListEncoding(structBuilder.getPreferredListEncoding());
    }
  }

private:
  NodeTranslator& translator;
  const ErrorReporter& errorReporter;
  StructLayout layout;
  kj::Arena arena;

  struct MemberInfo {
    MemberInfo* parent;
    // The MemberInfo for the parent scope.

    uint codeOrder;
    // Code order within the parent.

    uint index = 0;
    // Index within the parent.

    uint childCount = 0;
    // Number of children this member has.

    uint childInitializedCount = 0;
    // Number of children whose `schema` member has been initialized.  This initialization happens
    // while walking the fields in ordinal order.

    uint unionDiscriminantCount = 0;
    // Number of children who are members of the scope's union and have had their discriminant
    // value decided.

    bool isInUnion;
    // Whether or not this field is in the parent's union.

    Declaration::Reader decl;

    kj::Maybe<schema2::Field::Builder> schema;
    // Schema for the field.  Initialized when getSchema() is first called.

    schema2::Node::Builder node;
    // If it's a group, or the top-level struct.

    union {
      StructLayout::StructOrGroup* fieldScope;
      // If this member is a field, the scope of that field.  This will be used to assign an
      // offset for the field when going through in ordinal order.

      StructLayout::Union* unionScope;
      // If this member is a union, or it is a group or top-level struct containing an unnamed
      // union, this is the union.  This will be used to assign a discriminant offset when the
      // union's ordinal comes up (if the union has an explicit ordinal), as well as to finally
      // copy over the discriminant offset to the schema.
    };

    inline explicit MemberInfo(schema2::Node::Builder node)
        : parent(nullptr), codeOrder(0), isInUnion(false), node(node), unionScope(nullptr) {}
    inline MemberInfo(MemberInfo& parent, uint codeOrder,
                      const Declaration::Reader& decl,
                      StructLayout::StructOrGroup& fieldScope,
                      bool isInUnion)
        : parent(&parent), codeOrder(codeOrder), isInUnion(isInUnion),
          decl(decl), fieldScope(&fieldScope) {}
    inline MemberInfo(MemberInfo& parent, uint codeOrder,
                      const Declaration::Reader& decl,
                      schema2::Node::Builder node,
                      bool isInUnion)
        : parent(&parent), codeOrder(codeOrder), isInUnion(isInUnion),
          decl(decl), node(node), unionScope(nullptr) {}

    schema2::Field::Builder getSchema() {
      KJ_IF_MAYBE(result, schema) {
        return *result;
      } else {
        index = parent->childInitializedCount;
        auto builder = parent->addMemberSchema();
        if (isInUnion) {
          builder.setDiscriminantValue(parent->unionDiscriminantCount++);
        }
        builder.setName(decl.getName().getValue());
        builder.setCodeOrder(codeOrder);
        schema = builder;
        return builder;
      }
    }

    schema2::Field::Builder addMemberSchema() {
      // Get the schema builder for the child member at the given index.  This lazily/dynamically
      // builds the builder tree.

      KJ_REQUIRE(childInitializedCount < childCount);

      auto structNode = node.getStruct();
      if (!structNode.hasFields()) {
        if (parent != nullptr) {
          getSchema();  // Make sure field exists in parent once the first child is added.
        }
        return structNode.initFields(childCount)[childInitializedCount++];
      } else {
        return structNode.getFields()[childInitializedCount++];
      }
    }

    void finishGroup() {
      if (unionScope != nullptr) {
        unionScope->addDiscriminant();  // if it hasn't happened already
        auto structNode = node.getStruct();
        structNode.setDiscriminantCount(unionDiscriminantCount);
        structNode.setDiscriminantOffset(KJ_ASSERT_NONNULL(unionScope->discriminantOffset));
      }

      if (parent != nullptr) {
        uint64_t groupId = generateGroupId(parent->node.getId(), index);
        node.setId(groupId);
        getSchema().setGroup(groupId);
      }
    }
  };

  std::multimap<uint, MemberInfo*> membersByOrdinal;
  // Every member that has an explicit ordinal goes into this map.  We then iterate over the map
  // to assign field offsets (or discriminant offsets for unions).

  kj::Vector<MemberInfo*> allMembers;
  // All members, including ones that don't have ordinals.

  void traverseUnion(List<Declaration>::Reader members, MemberInfo& parent,
                     StructLayout::Union& layout, uint& codeOrder) {
    if (members.size() < 2) {
      errorReporter.addErrorOn(parent.decl, "Union must have at least two members.");
    }

    for (auto member: members) {
      kj::Maybe<uint> ordinal;
      MemberInfo* memberInfo = nullptr;

      switch (member.getBody().which()) {
        case Declaration::Body::FIELD_DECL: {
          parent.childCount++;
          // For layout purposes, pretend this field is enclosed in a one-member group.
          StructLayout::Group& singletonGroup =
              arena.allocate<StructLayout::Group>(layout);
          memberInfo = &arena.allocate<MemberInfo>(parent, codeOrder++, member, singletonGroup,
                                                   true);
          allMembers.add(memberInfo);
          ordinal = member.getId().getOrdinal().getValue();
          break;
        }

        case Declaration::Body::UNION_DECL:
          errorReporter.addErrorOn(member, "Unions cannot contain unions.");
          break;

        case Declaration::Body::GROUP_DECL: {
          parent.childCount++;
          StructLayout::Group& group = arena.allocate<StructLayout::Group>(layout);
          memberInfo = &arena.allocate<MemberInfo>(
              parent, codeOrder++, member,
              newGroupNode(parent.node, member.getName().getValue()),
              true);
          allMembers.add(memberInfo);
          traverseGroup(member.getNestedDecls(), *memberInfo, group);
          break;
        }

        default:
          // Ignore others.
          break;
      }

      KJ_IF_MAYBE(o, ordinal) {
        membersByOrdinal.insert(std::make_pair(*o, memberInfo));
      }
    }
  }

  void traverseGroup(List<Declaration>::Reader members, MemberInfo& parent,
                     StructLayout::StructOrGroup& layout) {
    if (members.size() < 1) {
      errorReporter.addErrorOn(parent.decl, "Group must have at least one member.");
    }

    traverseTopOrGroup(members, parent, layout);
  }

  void traverseTopOrGroup(List<Declaration>::Reader members, MemberInfo& parent,
                          StructLayout::StructOrGroup& layout) {
    uint codeOrder = 0;

    for (auto member: members) {
      kj::Maybe<uint> ordinal;
      MemberInfo* memberInfo = nullptr;

      switch (member.getBody().which()) {
        case Declaration::Body::FIELD_DECL: {
          parent.childCount++;
          memberInfo = &arena.allocate<MemberInfo>(
              parent, codeOrder++, member, layout, false);
          allMembers.add(memberInfo);
          ordinal = member.getId().getOrdinal().getValue();
          break;
        }

        case Declaration::Body::UNION_DECL: {
          StructLayout::Union& unionLayout = arena.allocate<StructLayout::Union>(layout);

          uint independentSubCodeOrder = 0;
          uint* subCodeOrder = &independentSubCodeOrder;
          if (member.getName().getValue() == "") {
            memberInfo = &parent;
            subCodeOrder = &codeOrder;
          } else {
            parent.childCount++;
            memberInfo = &arena.allocate<MemberInfo>(
                parent, codeOrder++, member,
                newGroupNode(parent.node, member.getName().getValue()),
                false);
            allMembers.add(memberInfo);
          }
          memberInfo->unionScope = &unionLayout;
          traverseUnion(member.getNestedDecls(), *memberInfo, unionLayout, *subCodeOrder);
          if (member.getId().which() == Declaration::Id::ORDINAL) {
            ordinal = member.getId().getOrdinal().getValue();
          }
          break;
        }

        case Declaration::Body::GROUP_DECL:
          parent.childCount++;
          memberInfo = &arena.allocate<MemberInfo>(
              parent, codeOrder++, member,
              newGroupNode(parent.node, member.getName().getValue()),
              false);
          allMembers.add(memberInfo);

          // Members of the group are laid out just like they were members of the parent, so we
          // just pass along the parent layout.
          traverseGroup(member.getNestedDecls(), *memberInfo, layout);

          // No ordinal for groups.
          break;

        default:
          // Ignore others.
          break;
      }

      KJ_IF_MAYBE(o, ordinal) {
        membersByOrdinal.insert(std::make_pair(*o, memberInfo));
      }
    }
  }

  schema2::Node::Builder newGroupNode(schema2::Node::Reader parent, kj::StringPtr name) {
    auto orphan = Orphanage::getForMessageContaining(translator.wipNode.get())
        .newOrphan<schema2::Node>();
    auto node = orphan.get();

    // We'll set the ID later.
    node.setDisplayName(kj::str(parent.getDisplayName(), '.', name));
    node.setDisplayNamePrefixLength(node.getDisplayName().size() - name.size());
    node.setScopeId(parent.getId());
    node.initStruct().setIsGroup(true);

    // The remaining contents of node.struct will be filled in later.

    translator.groups.add(kj::mv(orphan));
    return node;
  }
};

void NodeTranslator::compileStruct(Declaration::Struct::Reader decl,
                                   List<Declaration>::Reader members,
                                   schema2::Node::Builder builder) {
  StructTranslator(*this).translate(decl, members, builder);
}

// -------------------------------------------------------------------

void NodeTranslator::compileInterface(Declaration::Interface::Reader decl,
                                      List<Declaration>::Reader members,
                                      schema2::Node::Builder builder) {
  KJ_FAIL_ASSERT("TODO: compile interfaces");
}

// -------------------------------------------------------------------

static kj::String declNameString(DeclName::Reader name) {
  kj::String prefix;

  switch (name.getBase().which()) {
    case DeclName::Base::RELATIVE_NAME:
      prefix = kj::str(name.getBase().getRelativeName());
      break;
    case DeclName::Base::ABSOLUTE_NAME:
      prefix = kj::str(".", name.getBase().getAbsoluteName());
      break;
    case DeclName::Base::IMPORT_NAME:
      prefix = kj::str("import \"", name.getBase().getImportName(), "\"");
      break;
  }

  if (name.getMemberPath().size() == 0) {
    return prefix;
  } else {
    auto path = name.getMemberPath();
    KJ_STACK_ARRAY(kj::StringPtr, parts, path.size(), 16, 16);
    for (size_t i = 0; i < parts.size(); i++) {
      parts[i] = path[i].getValue();
    }
    return kj::str(prefix, ".", kj::strArray(parts, "."));
  }
}

bool NodeTranslator::compileType(TypeExpression::Reader source, schema2::Type::Builder target) {
  auto name = source.getName();
  KJ_IF_MAYBE(base, resolver.resolve(name)) {
    bool handledParams = false;

    switch (base->kind) {
      case Declaration::Body::ENUM_DECL: target.setEnum(base->id); break;
      case Declaration::Body::STRUCT_DECL: target.setStruct(base->id); break;
      case Declaration::Body::INTERFACE_DECL: target.setInterface(base->id); break;

      case Declaration::Body::BUILTIN_LIST: {
        auto params = source.getParams();
        if (params.size() != 1) {
          errorReporter.addErrorOn(source, "'List' requires exactly one parameter.");
          return false;
        }

        auto elementType = target.initList();
        if (!compileType(params[0], elementType)) {
          return false;
        }

        if (elementType.which() == schema2::Type::OBJECT) {
          errorReporter.addErrorOn(source, "'List(Object)' is not supported.");
          // Seeing List(Object) later can mess things up, so change the type to Void.
          elementType.setVoid();
          return false;
        }

        handledParams = true;
        break;
      }

      case Declaration::Body::BUILTIN_VOID: target.setVoid(); break;
      case Declaration::Body::BUILTIN_BOOL: target.setBool(); break;
      case Declaration::Body::BUILTIN_INT8: target.setInt8(); break;
      case Declaration::Body::BUILTIN_INT16: target.setInt16(); break;
      case Declaration::Body::BUILTIN_INT32: target.setInt32(); break;
      case Declaration::Body::BUILTIN_INT64: target.setInt64(); break;
      case Declaration::Body::BUILTIN_U_INT8: target.setUint8(); break;
      case Declaration::Body::BUILTIN_U_INT16: target.setUint16(); break;
      case Declaration::Body::BUILTIN_U_INT32: target.setUint32(); break;
      case Declaration::Body::BUILTIN_U_INT64: target.setUint64(); break;
      case Declaration::Body::BUILTIN_FLOAT32: target.setFloat32(); break;
      case Declaration::Body::BUILTIN_FLOAT64: target.setFloat64(); break;
      case Declaration::Body::BUILTIN_TEXT: target.setText(); break;
      case Declaration::Body::BUILTIN_DATA: target.setData(); break;
      case Declaration::Body::BUILTIN_OBJECT: target.setObject(); break;

      default:
        errorReporter.addErrorOn(source, kj::str("'", declNameString(name), "' is not a type."));
        return false;
    }

    if (!handledParams) {
      if (source.getParams().size() != 0) {
        errorReporter.addErrorOn(source, kj::str(
            "'", declNameString(name), "' does not accept parameters."));
        return false;
      }
    }

    return true;

  } else {
    target.setVoid();
    return false;
  }
}

// -------------------------------------------------------------------

void NodeTranslator::compileDefaultDefaultValue(
    schema2::Type::Reader type, schema2::Value::Builder target) {
  switch (type.which()) {
    case schema2::Type::VOID: target.setVoid(); break;
    case schema2::Type::BOOL: target.setBool(false); break;
    case schema2::Type::INT8: target.setInt8(0); break;
    case schema2::Type::INT16: target.setInt16(0); break;
    case schema2::Type::INT32: target.setInt32(0); break;
    case schema2::Type::INT64: target.setInt64(0); break;
    case schema2::Type::UINT8: target.setUint8(0); break;
    case schema2::Type::UINT16: target.setUint16(0); break;
    case schema2::Type::UINT32: target.setUint32(0); break;
    case schema2::Type::UINT64: target.setUint64(0); break;
    case schema2::Type::FLOAT32: target.setFloat32(0); break;
    case schema2::Type::FLOAT64: target.setFloat64(0); break;
    case schema2::Type::ENUM: target.setEnum(0); break;
    case schema2::Type::INTERFACE: target.setInterface(); break;

    // Bit of a hack:  For "Object" types, we adopt a null orphan, which sets the field to null.
    // TODO(cleanup):  Create a cleaner way to do this.
    case schema2::Type::TEXT: target.adoptText(Orphan<Text>()); break;
    case schema2::Type::DATA: target.adoptData(Orphan<Data>()); break;
    case schema2::Type::STRUCT: target.adoptStruct(Orphan<Data>()); break;
    case schema2::Type::LIST: target.adoptList(Orphan<Data>()); break;
    case schema2::Type::OBJECT: target.adoptObject(Orphan<Data>()); break;
  }
}

class NodeTranslator::DynamicSlot {
  // Acts like a pointer to a field or list element.  The target's value can be set or initialized.
  // This is useful when recursively compiling values.
  //
  // TODO(someday):  The Dynamic API should support something like this directly.

public:
  DynamicSlot(DynamicStruct::Builder structBuilder, StructSchema::Field field)
      : type(FIELD), struct_{structBuilder, field} {}
  DynamicSlot(DynamicList::Builder listBuilder, uint index)
      : type(ELEMENT), list{listBuilder, index} {}
  DynamicSlot(DynamicStruct::Builder structBuilder, StructSchema::Field field,
              StructSchema structFieldSchema)
      : type(STRUCT_OBJECT_FIELD), struct_{structBuilder, field},
        structFieldSchema(structFieldSchema) {}
  DynamicSlot(DynamicStruct::Builder structBuilder, StructSchema::Field field,
              ListSchema listFieldSchema)
      : type(LIST_OBJECT_FIELD), struct_{structBuilder, field},
        listFieldSchema(listFieldSchema) {}
  DynamicSlot(DynamicStruct::Builder structBuilder, StructSchema::Field field,
              EnumSchema enumFieldSchema)
      : type(RAW_ENUM_FIELD), struct_{structBuilder, field},
        enumFieldSchema(enumFieldSchema) {}

  DynamicStruct::Builder initStruct() {
    switch (type) {
      case FIELD: return struct_.builder.init(struct_.field).as<DynamicStruct>();
      case ELEMENT: return list.builder[list.index].as<DynamicStruct>();
      case STRUCT_OBJECT_FIELD:
        return struct_.builder.initObject(struct_.field, structFieldSchema);
      case LIST_OBJECT_FIELD: KJ_FAIL_REQUIRE("Type mismatch.");
      case RAW_ENUM_FIELD: KJ_FAIL_REQUIRE("Type mismatch.");
    }
    KJ_FAIL_ASSERT("can't get here");
  }

  DynamicList::Builder initList(uint size) {
    switch (type) {
      case FIELD: return struct_.builder.init(struct_.field, size).as<DynamicList>();
      case ELEMENT: return list.builder.init(list.index, size).as<DynamicList>();
      case STRUCT_OBJECT_FIELD: KJ_FAIL_REQUIRE("Type mismatch.");
      case LIST_OBJECT_FIELD:
        return struct_.builder.initObject(struct_.field, listFieldSchema, size);
      case RAW_ENUM_FIELD: KJ_FAIL_REQUIRE("Type mismatch.");
    }
    KJ_FAIL_ASSERT("can't get here");
  }

  void set(DynamicValue::Reader value) {
    switch (type) {
      case FIELD: struct_.builder.set(struct_.field, value); return;
      case ELEMENT: list.builder.set(list.index, value); return;
      case STRUCT_OBJECT_FIELD: struct_.builder.set(struct_.field, value); return;
      case LIST_OBJECT_FIELD: struct_.builder.set(struct_.field, value); return;
      case RAW_ENUM_FIELD:
        struct_.builder.set(struct_.field, value.as<DynamicEnum>().getRaw());
        return;
    }
    KJ_FAIL_ASSERT("can't get here");
  }

  kj::Maybe<uint64_t> getEnumType() {
    // If the slot type is an enum, get its type ID.  Otherwise return nullptr.
    //
    // This is really ugly.

    switch (type) {
      case FIELD: return enumIdForField(struct_.field);
      case ELEMENT: {
        if (list.builder.getSchema().whichElementType() == schema2::Type::ENUM) {
          return list.builder.getSchema().getEnumElementType().getProto().getId();
        }
        return nullptr;
      }
      case STRUCT_OBJECT_FIELD: return nullptr;
      case LIST_OBJECT_FIELD: return nullptr;
      case RAW_ENUM_FIELD: return enumFieldSchema.getProto().getId();
    }
    KJ_FAIL_ASSERT("can't get here");
  }

private:
  enum Type {
    FIELD, ELEMENT, STRUCT_OBJECT_FIELD, LIST_OBJECT_FIELD, RAW_ENUM_FIELD
  };
  Type type;

  union {
    struct {
      DynamicStruct::Builder builder;
      StructSchema::Field field;
    } struct_;
    struct {
      DynamicList::Builder builder;
      uint index;
    } list;
  };

  union {
    StructSchema structFieldSchema;
    ListSchema listFieldSchema;
    EnumSchema enumFieldSchema;
  };

  static kj::Maybe<uint64_t> enumIdForField(StructSchema::Field field) {
    schema2::Field::Reader proto = field.getProto();
    if (proto.which() == schema2::Field::REGULAR) {
      auto type = proto.getRegular().getType();
      if (type.which() == schema2::Type::ENUM) {
        return type.getEnum();
      }
    }
    return nullptr;
  }
};

static kj::StringPtr getValueUnionFieldNameFor(schema2::Type::Which type) {
  switch (type) {
    case schema2::Type::VOID: return "voidValue";
    case schema2::Type::BOOL: return "boolValue";
    case schema2::Type::INT8: return "int8Value";
    case schema2::Type::INT16: return "int16Value";
    case schema2::Type::INT32: return "int32Value";
    case schema2::Type::INT64: return "int64Value";
    case schema2::Type::UINT8: return "uint8Value";
    case schema2::Type::UINT16: return "uint16Value";
    case schema2::Type::UINT32: return "uint32Value";
    case schema2::Type::UINT64: return "uint64Value";
    case schema2::Type::FLOAT32: return "float32Value";
    case schema2::Type::FLOAT64: return "float64Value";
    case schema2::Type::TEXT: return "textValue";
    case schema2::Type::DATA: return "dataValue";
    case schema2::Type::LIST: return "listValue";
    case schema2::Type::ENUM: return "enumValue";
    case schema2::Type::STRUCT: return "structValue";
    case schema2::Type::INTERFACE: return "interfaceValue";
    case schema2::Type::OBJECT: return "objectValue";
  }
  KJ_FAIL_ASSERT("Unknown type.");
}

void NodeTranslator::compileBootstrapValue(ValueExpression::Reader source,
                                           schema2::Type::Reader type,
                                           schema2::Value::Builder target) {
  // Start by filling in a default default value so that if for whatever reason we don't end up
  // initializing the value, this won't cause schema validation to fail.
  compileDefaultDefaultValue(type, target);

  switch (type.which()) {
    case schema2::Type::LIST:
    case schema2::Type::STRUCT:
    case schema2::Type::INTERFACE:
    case schema2::Type::OBJECT:
      unfinishedValues.add(UnfinishedValue { source, type, target });
      break;

    default:
      // Primitive value.
      compileValue(source, type, target, true);
      break;
  }
}

void NodeTranslator::compileValue(ValueExpression::Reader source, schema2::Type::Reader type,
                                  schema2::Value::Builder target, bool isBootstrap) {
#warning "temporary hack for schema transition"
  switch (type.which()) {
    case schema2::Type::TEXT:
      target.setText(source.getBody().getString());
      break;

    case schema2::Type::UINT16:
      target.setUint16(source.getBody().getPositiveInt());
      break;

    default:
      KJ_FAIL_ASSERT("Need to compile value type:", (uint)type.which(),
                     wipNode.getReader().getDisplayName());
  }

#if 0
  auto valueUnion = toDynamic(target).get("body").as<DynamicUnion>();
  auto member = valueUnion.getSchema().getMemberByName(
      getValueUnionFieldNameFor(type.getBody().which()));
  switch (type.getBody().which()) {
    case schema2::Type::LIST:
      KJ_IF_MAYBE(listSchema, makeListSchemaOf(type.getBody().getListType())) {
        DynamicSlot slot(valueUnion, member, *listSchema);
        compileValue(source, slot, isBootstrap);
      }
      break;
    case schema2::Type::STRUCT:
      KJ_IF_MAYBE(structSchema, resolver.resolveBootstrapSchema(type.getBody().getStructType())) {
        DynamicSlot slot(valueUnion, member, structSchema->asStruct());
        compileValue(source, slot, isBootstrap);
      }
      break;
    case schema2::Type::ENUM:
      KJ_IF_MAYBE(enumSchema, resolver.resolveBootstrapSchema(type.getBody().getEnumType())) {
        DynamicSlot slot(valueUnion, member, enumSchema->asEnum());
        compileValue(source, slot, isBootstrap);
      }
      break;
    default:
      DynamicSlot slot(valueUnion, member);
      compileValue(source, slot, isBootstrap);
      break;
  }
#endif
}

void NodeTranslator::compileValue(ValueExpression::Reader src, DynamicSlot& dst, bool isBootstrap) {
  // We rely on the dynamic API to detect type errors and throw exceptions.
  //
  // TODO(cleanup):  We should perhaps ensure that all exceptions that this might throw are
  //   recoverable, so that this doesn't crash if -fno-exceptions is enabled.  Or create a better
  //   way to test for type compatibility without throwing.
  KJ_IF_MAYBE(exception, kj::runCatchingExceptions(
      [&]() { compileValueInner(src, dst, isBootstrap); })) {
    errorReporter.addErrorOn(src, "Type mismatch.");
  }
}

void NodeTranslator::compileValueInner(
    ValueExpression::Reader src, DynamicSlot& dst, bool isBootstrap) {
  switch (src.getBody().which()) {
    case ValueExpression::Body::NAME: {
      auto name = src.getBody().getName();
      bool isBare = name.getBase().which() == DeclName::Base::RELATIVE_NAME &&
                    name.getMemberPath().size() == 0;
      bool wasSet = false;
      if (isBare) {
        // The name is just a bare identifier.  It may be a literal value or an enumerant.
        kj::StringPtr id = name.getBase().getRelativeName().getValue();

        KJ_IF_MAYBE(enumId, dst.getEnumType()) {
          KJ_IF_MAYBE(enumSchema, resolver.resolveBootstrapSchema(*enumId)) {
            KJ_IF_MAYBE(enumerant, enumSchema->asEnum().findEnumerantByName(id)) {
              dst.set(DynamicEnum(*enumerant));
              wasSet = true;
            }
          } else {
            // Enum type is broken.  We don't want to report a redundant error here, so just assume
            // we would have found a matching enumerant.
            dst.set(kj::implicitCast<uint16_t>(0));
            wasSet = true;
          }
        } else {
          // Interpret known constant values.
          if (id == "void") {
            dst.set(Void::VOID);
            wasSet = true;
          } else if (id == "true") {
            dst.set(true);
            wasSet = true;
          } else if (id == "false") {
            dst.set(false);
            wasSet = true;
          } else if (id == "nan") {
            dst.set(std::numeric_limits<double>::quiet_NaN());
            wasSet = true;
          } else if (id == "inf") {
            dst.set(std::numeric_limits<double>::infinity());
            wasSet = true;
          }
        }
      }

      if (!wasSet) {
        // Haven't resolved the name yet.  Try looking up a constant.
        KJ_IF_MAYBE(constValue, readConstant(src.getBody().getName(), isBootstrap, src)) {
          dst.set(*constValue);
        }
      }
      break;
    }

    case ValueExpression::Body::POSITIVE_INT:
      dst.set(src.getBody().getPositiveInt());
      break;

    case ValueExpression::Body::NEGATIVE_INT: {
      uint64_t nValue = src.getBody().getNegativeInt();
      if (nValue > (std::numeric_limits<uint64_t>::max() >> 1) + 1) {
        errorReporter.addErrorOn(src, "Integer is too big to be negative.");
      } else {
        dst.set(kj::implicitCast<int64_t>(-nValue));
      }
      break;
    }

    case ValueExpression::Body::FLOAT:
      dst.set(src.getBody().getFloat());
      break;

    case ValueExpression::Body::STRING:
      dst.set(src.getBody().getString());
      break;

    case ValueExpression::Body::LIST: {
      auto srcList = src.getBody().getList();
      auto dstList = dst.initList(srcList.size());
      for (uint i = 0; i < srcList.size(); i++) {
        DynamicSlot slot(dstList, i);
        compileValue(srcList[i], slot, isBootstrap);
      }
      break;
    }

    case ValueExpression::Body::STRUCT_VALUE: {
      auto srcStruct = src.getBody().getStructValue();
      auto dstStruct = dst.initStruct();
      auto dstSchema = dstStruct.getSchema();
      for (auto assignment: srcStruct) {
        auto fieldName = assignment.getFieldName();

        switch (assignment.which()) {
          case ValueExpression::FieldAssignment::NOT_UNION:
            KJ_IF_MAYBE(field, dstSchema.findFieldByName(fieldName.getValue())) {
              DynamicSlot slot(dstStruct, *field);
              compileValue(assignment.getValue(), slot, isBootstrap);
            } else {
              errorReporter.addErrorOn(fieldName, kj::str(
                  "Struct has no field named '", fieldName.getValue(), "'."));
            }
            break;

          case ValueExpression::FieldAssignment::UNION: {
            KJ_FAIL_ASSERT("Union literal syntax is obsolete.");
            break;
          }
        }
      }
      break;
    }

    case ValueExpression::Body::UNKNOWN:
      // Ignore earlier error.
      break;
  }
}

kj::Maybe<DynamicValue::Reader> NodeTranslator::readConstant(
    DeclName::Reader name, bool isBootstrap, ValueExpression::Reader errorLocation) {
  KJ_IF_MAYBE(resolved, resolver.resolve(name)) {
    if (resolved->kind != Declaration::Body::CONST_DECL) {
      errorReporter.addErrorOn(errorLocation,
          kj::str("'", declNameString(name), "' does not refer to a constant."));
      return nullptr;
    }

    // If we're bootstrapping, then we know we're expecting a primitive value, so if the
    // constant turns out to be non-primitive, we'll error out anyway.  If we're not
    // bootstrapping, we may be compiling a non-primitive value and so we need the final
    // version of the constant to make sure its value is filled in.
    kj::Maybe<schema2::Node::Reader> maybeConstSchema = isBootstrap ?
        resolver.resolveBootstrapSchema(resolved->id).map([](Schema s) { return s.getProto(); }) :
        resolver.resolveFinalSchema(resolved->id);
    KJ_IF_MAYBE(constSchema, maybeConstSchema) {
      auto constReader = constSchema->getConst();
      auto dynamicConst = toDynamic(constReader.getValue());
      auto constValue = dynamicConst.get(KJ_ASSERT_NONNULL(dynamicConst.which()));

      if (constValue.getType() == DynamicValue::OBJECT) {
        // We need to assign an appropriate schema to this object.
        DynamicObject objValue = constValue.as<DynamicObject>();
        auto constType = constReader.getType();
        switch (constType.which()) {
          case schema2::Type::STRUCT:
            KJ_IF_MAYBE(structSchema, resolver.resolveBootstrapSchema(constType.getStruct())) {
              constValue = objValue.as(structSchema->asStruct());
            } else {
              // The struct's schema is broken for reasons already reported.
              return nullptr;
            }
            break;
          case schema2::Type::LIST:
            KJ_IF_MAYBE(listSchema, makeListSchemaOf(constType.getList())) {
              constValue = objValue.as(*listSchema);
            } else {
              // The list's schema is broken for reasons already reported.
              return nullptr;
            }
            break;
          case schema2::Type::OBJECT:
            // Fine as-is.
            break;
          default:
            KJ_FAIL_ASSERT("Unrecognized Object-typed member of schema::Value.");
            break;
        }
      }

      if (name.getBase().which() == DeclName::Base::RELATIVE_NAME &&
          name.getMemberPath().size() == 0) {
        // A fully unqualified identifier looks like it might refer to a constant visible in the
        // current scope, but if that's really what the user wanted, we want them to use a
        // qualified name to make it more obvious.  Report an error.
        KJ_IF_MAYBE(scope, resolver.resolveBootstrapSchema(constSchema->getScopeId())) {
          auto scopeReader = scope->getProto();
          kj::StringPtr parent;
          if (scopeReader.which() == schema2::Node::FILE) {
            parent = "";
          } else {
            parent = scopeReader.getDisplayName().slice(scopeReader.getDisplayNamePrefixLength());
          }
          kj::StringPtr id = name.getBase().getRelativeName().getValue();

          errorReporter.addErrorOn(errorLocation, kj::str(
              "Constant names must be qualified to avoid confusion.  Please replace '",
              declNameString(name), "' with '", parent, ".", id,
              "', if that's what you intended."));
        }
      }

      return constValue;
    } else {
      // The target is a constant, but the constant's schema is broken for reasons already reported.
      return nullptr;
    }
  } else {
    // Lookup will have reported an error.
    return nullptr;
  }
}

kj::Maybe<ListSchema> NodeTranslator::makeListSchemaOf(schema2::Type::Reader elementType) {
  switch (elementType.which()) {
    case schema2::Type::ENUM:
      KJ_IF_MAYBE(enumSchema, resolver.resolveBootstrapSchema(elementType.getEnum())) {
        return ListSchema::of(enumSchema->asEnum());
      } else {
        return nullptr;
      }
    case schema2::Type::STRUCT:
      KJ_IF_MAYBE(structSchema, resolver.resolveBootstrapSchema(elementType.getStruct())) {
        return ListSchema::of(structSchema->asStruct());
      } else {
        return nullptr;
      }
    case schema2::Type::INTERFACE:
      KJ_IF_MAYBE(interfaceSchema, resolver.resolveBootstrapSchema(elementType.getInterface())) {
        return ListSchema::of(interfaceSchema->asInterface());
      } else {
        return nullptr;
      }
    case schema2::Type::LIST:
      KJ_IF_MAYBE(listSchema, makeListSchemaOf(elementType.getList())) {
        return ListSchema::of(*listSchema);
      } else {
        return nullptr;
      }
    default:
      return ListSchema::of(elementType.which());
  }
}

Orphan<List<schema2::Annotation>> NodeTranslator::compileAnnotationApplications(
    List<Declaration::AnnotationApplication>::Reader annotations,
    kj::StringPtr targetsFlagName) {
  if (annotations.size() == 0 || !compileAnnotations) {
    // Return null.
    return Orphan<List<schema2::Annotation>>();
  }

  Orphanage orphanage = Orphanage::getForMessageContaining(wipNode.get());
  auto result = orphanage.newOrphan<List<schema2::Annotation>>(annotations.size());
  auto builder = result.get();

  for (uint i = 0; i < annotations.size(); i++) {
    Declaration::AnnotationApplication::Reader annotation = annotations[i];
    schema2::Annotation::Builder annotationBuilder = builder[i];

    // Set the annotation's value to void in case we fail to produce something better below.
    annotationBuilder.initValue().setVoid();

    auto name = annotation.getName();
    KJ_IF_MAYBE(decl, resolver.resolve(name)) {
      if (decl->kind != Declaration::Body::ANNOTATION_DECL) {
        errorReporter.addErrorOn(name, kj::str(
            "'", declNameString(name), "' is not an annotation."));
      } else {
        annotationBuilder.setId(decl->id);
        KJ_IF_MAYBE(annotationSchema, resolver.resolveBootstrapSchema(decl->id)) {
          auto node = annotationSchema->getProto().getAnnotation();
#warning "temporary hack for schema transition"
#if 0
          if (!toDynamic(node).get(targetsFlagName).as<bool>()) {
            errorReporter.addErrorOn(name, kj::str(
                "'", declNameString(name), "' cannot be applied to this kind of declaration."));
          }
#endif

          // Interpret the value.
          auto value = annotation.getValue();
          switch (value.which()) {
            case Declaration::AnnotationApplication::Value::NONE:
              // No value, i.e. void.
              if (node.getType().which() == schema2::Type::VOID) {
                annotationBuilder.getValue().setVoid();
              } else {
                errorReporter.addErrorOn(name, kj::str(
                    "'", declNameString(name), "' requires a value."));
                compileDefaultDefaultValue(node.getType(), annotationBuilder.getValue());
              }
              break;

            case Declaration::AnnotationApplication::Value::EXPRESSION:
              compileBootstrapValue(value.getExpression(), node.getType(),
                                    annotationBuilder.getValue());
              break;
          }
        }
      }
    }
  }

  return result;
}

}  // namespace compiler
}  // namespace capnp
