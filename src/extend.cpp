#include "sass.hpp"
#include "extend.hpp"
#include "context.hpp"
#include "backtrace.hpp"
#include "paths.hpp"
#include "parser.hpp"
#include "node.hpp"
#include "sass_util.hpp"
#include "remove_placeholders.hpp"
#include "debug.hpp"
#include <iostream>
#include <deque>
#include <set>


/*
 NOTES:

 - The print* functions print to cerr. This allows our testing frameworks (like sass-spec) to ignore the output, which
   is very helpful when debugging. The format of the output is mainly to wrap things in square brackets to match what
   ruby already outputs (to make comparisons easier).

 - For the direct porting effort, we're trying to port method-for-method until we get all the tests passing.
   Where applicable, I've tried to include the ruby code above the function for reference until all our tests pass.
   The ruby code isn't always directly portable, so I've tried to include any modified ruby code that was actually
   used for the porting.

 - DO NOT try to optimize yet. We get a tremendous benefit out of comparing the output of each stage of the extend to the ruby
   output at the same stage. This makes it much easier to determine where problems are. Try to keep as close to
   the ruby code as you can until we have all the sass-spec tests passing. Then, we should optimize. However, if you see
   something that could probably be optimized, let's not forget it. Add a // TODO: or // IMPROVEMENT: comment.

 - Coding conventions in this file (these may need to be changed before merging back into master)
   - Very basic hungarian notation:
     p prefix for pointers (pSelector)
     no prefix for value types and references (selector)
   - Use STL iterators where possible
   - prefer verbose naming over terse naming
   - use typedefs for STL container types for make maintenance easier

 - You may see a lot of comments that say "// TODO: is this the correct combinator?". See the comment referring to combinators
   in extendCompoundSelector for a more extensive explanation of my confusion. I think our divergence in data model from ruby
   sass causes this to be necessary.


 GLOBAL TODOS:

 - wrap the contents of the print functions in DEBUG preprocesser conditionals so they will be optimized away in non-debug mode.

 - consider making the extend* functions member functions to avoid passing around ctx and subset_map map around. This has the
   drawback that the implementation details of the operator are then exposed to the outside world, which is not ideal and
   can cause additional compile time dependencies.

 - mark the helper methods in this file static to given them compilation unit linkage.

 - implement parent directive matching

 - fix compilation warnings for unused Extend members if we really don't need those references anymore.
 */


namespace Sass {


  typedef std::pair<Complex_Selector*, Compound_Selector*> ExtensionPair;
  typedef std::vector<ExtensionPair> SubsetMapEntries;

#ifdef DEBUG

  // TODO: move the ast specific ostream operators into ast.hpp/ast.cpp
  std::ostream& operator<<(std::ostream& os, const Complex_Selector::Combinator combinator) {
    switch (combinator) {
      case Complex_Selector::ANCESTOR_OF: os << "\" \""; break;
      case Complex_Selector::PARENT_OF:   os << "\">\""; break;
      case Complex_Selector::PRECEDES:    os << "\"~\""; break;
      case Complex_Selector::ADJACENT_TO: os << "\"+\""; break;
      case Complex_Selector::REFERENCE: os    << "\"/\""; break;
    }

    return os;
  }


  std::ostream& operator<<(std::ostream& os, Compound_Selector& compoundSelector) {
    for (size_t i = 0, L = compoundSelector.length(); i < L; ++i) {
      if (i > 0) os << ", ";
      os << compoundSelector[i]->to_string();
    }
    return os;
  }

  std::ostream& operator<<(std::ostream& os, Simple_Selector& simpleSelector) {
    os << simpleSelector.to_string();
    return os;
  }

  // Print a string representation of a Compound_Selector
  static void printSimpleSelector(Simple_Selector* pSimpleSelector, const char* message=NULL, bool newline=true) {

    if (message) {
      std::cerr << message;
    }

    if (pSimpleSelector) {
      std::cerr << "[" << *pSimpleSelector << "]";
    } else {
      std::cerr << "NULL";
    }

    if (newline) {
      std::cerr << std::endl;
    }
  }

  // Print a string representation of a Compound_Selector
    typedef std::pair<Compound_Selector*, Complex_Selector*> SelsNewSeqPair;
    typedef std::vector<SelsNewSeqPair> SelsNewSeqPairCollection;


  // Print a string representation of a Compound_Selector
  static void printCompoundSelector(Compound_Selector* pCompoundSelector, const char* message=NULL, bool newline=true) {

    if (message) {
      std::cerr << message;
    }

    if (pCompoundSelector) {
      std::cerr << "[" << *pCompoundSelector << "]";
    } else {
      std::cerr << "NULL";
    }

    if (newline) {
      std::cerr << std::endl;
    }
  }


  std::ostream& operator<<(std::ostream& os, Complex_Selector& complexSelector) {

    os << "[";
    Complex_Selector* pIter = &complexSelector;
    bool first = true;
    while (pIter) {
      if (pIter->combinator() != Complex_Selector::ANCESTOR_OF) {
        if (!first) {
          os << ", ";
        }
        first = false;
        os << pIter->combinator();
      }

      if (!first) {
        os << ", ";
      }
      first = false;

      if (pIter->head()) {
        os << pIter->head()->to_string();
      } else {
        os << "NULL_HEAD";
      }

      pIter = pIter->tail();
    }
    os << "]";

    return os;
  }


  // Print a string representation of a Complex_Selector
  static void printComplexSelector(Complex_Selector* pComplexSelector, const char* message=NULL, bool newline=true) {

    if (message) {
      std::cerr << message;
    }

    if (pComplexSelector) {
      std::cerr << *pComplexSelector;
    } else {
      std::cerr << "NULL";
    }

    if (newline) {
      std::cerr << std::endl;
    }
  }

  static void printSelsNewSeqPairCollection(SelsNewSeqPairCollection& collection, const char* message=NULL, bool newline=true) {

    if (message) {
      std::cerr << message;
    }
    bool first = true;
    std::cerr << "[";
    for(SelsNewSeqPair& pair : collection) {
      if (first) {
        first = false;
      } else {
        std::cerr << ", ";
      }
      std::cerr << "[";
      Compound_Selector* pSels = pair.first;
      Complex_Selector* pNewSelector = pair.second;
      std::cerr << "[" << *pSels << "], ";
      printComplexSelector(pNewSelector, NULL, false);
    }
    std::cerr << "]";

    if (newline) {
      std::cerr << std::endl;
    }
  }

  // Print a string representation of a SourcesSet
  static void printSourcesSet(SourcesSet& sources, Context& ctx, const char* message=NULL, bool newline=true) {

    if (message) {
      std::cerr << message;
    }

    // Convert to a deque of strings so we can sort since order doesn't matter in a set. This should cut down on
    // the differences we see when debug printing.
    typedef std::deque<std::string> SourceStrings;
    SourceStrings sourceStrings;
    for (SourcesSet::iterator iterator = sources.begin(), iteratorEnd = sources.end(); iterator != iteratorEnd; ++iterator) {
      Complex_Selector* pSource = *iterator;
      std::stringstream sstream;
      sstream << complexSelectorToNode(pSource, ctx);
      sourceStrings.push_back(sstream.str());
    }

    // Sort to get consistent output
    std::sort(sourceStrings.begin(), sourceStrings.end());

    std::cerr << "SourcesSet[";
    for (SourceStrings::iterator iterator = sourceStrings.begin(), iteratorEnd = sourceStrings.end(); iterator != iteratorEnd; ++iterator) {
      std::string source = *iterator;
      if (iterator != sourceStrings.begin()) {
        std::cerr << ", ";
      }
      std::cerr << source;
    }
    std::cerr << "]";

    if (newline) {
      std::cerr << std::endl;
    }
  }


  std::ostream& operator<<(std::ostream& os, SubsetMapEntries& entries) {
    os << "SUBSET_MAP_ENTRIES[";

    for (SubsetMapEntries::iterator iterator = entries.begin(), endIterator = entries.end(); iterator != endIterator; ++iterator) {
      Complex_Selector* pExtComplexSelector = iterator->first;    // The selector up to where the @extend is (ie, the thing to merge)
      Compound_Selector* pExtCompoundSelector = iterator->second; // The stuff after the @extend

      if (iterator != entries.begin()) {
        os << ", ";
      }

      os << "(";

      if (pExtComplexSelector) {
        std::cerr << *pExtComplexSelector;
      } else {
        std::cerr << "NULL";
      }

      os << " -> ";

      if (pExtCompoundSelector) {
        std::cerr << *pExtCompoundSelector;
      } else {
        std::cerr << "NULL";
      }

      os << ")";

    }

    os << "]";

    return os;
  }
#endif

  static bool parentSuperselector(Complex_Selector* pOne, Complex_Selector* pTwo, Context& ctx) {
    // TODO: figure out a better way to create a Complex_Selector from scratch
    // TODO: There's got to be a better way. This got ugly quick...
    Position noPosition(-1, -1, -1);
    Type_Selector fakeParent(ParserState("[FAKE]"), "temp");
    Compound_Selector fakeHead(ParserState("[FAKE]"), 1 /*size*/);
    fakeHead.elements().push_back(&fakeParent);
    Complex_Selector fakeParentContainer(ParserState("[FAKE]"), Complex_Selector::ANCESTOR_OF, &fakeHead /*head*/, NULL /*tail*/);

    pOne->set_innermost(&fakeParentContainer, Complex_Selector::ANCESTOR_OF);
    pTwo->set_innermost(&fakeParentContainer, Complex_Selector::ANCESTOR_OF);

    bool isSuperselector = pOne->is_superselector_of(pTwo);

    pOne->clear_innermost();
    pTwo->clear_innermost();

    return isSuperselector;
  }

  void nodeToComplexSelectorDeque(const Node& node, ComplexSelectorDeque& out, Context& ctx) {
    for (NodeDeque::iterator iter = node.collection()->begin(), iterEnd = node.collection()->end(); iter != iterEnd; iter++) {
      Node& child = *iter;
      out.push_back(nodeToComplexSelector(child, ctx));
    }
  }

  Node complexSelectorDequeToNode(const ComplexSelectorDeque& deque, Context& ctx) {
    Node result = Node::createCollection();

    for (ComplexSelectorDeque::const_iterator iter = deque.begin(), iterEnd = deque.end(); iter != iterEnd; iter++) {
      Complex_Selector* pChild = *iter;
      result.collection()->push_back(complexSelectorToNode(pChild, ctx));
    }

    return result;
  }

  class LcsCollectionComparator {
  public:
    LcsCollectionComparator(Context& ctx) : mCtx(ctx) {}

    Context& mCtx;

    bool operator()(Complex_Selector* pOne, Complex_Selector* pTwo, Complex_Selector*& pOut) const {
      /*
      This code is based on the following block from ruby sass' subweave
        do |s1, s2|
          next s1 if s1 == s2
          next unless s1.first.is_a?(SimpleSequence) && s2.first.is_a?(SimpleSequence)
          next s2 if parent_superselector?(s1, s2)
          next s1 if parent_superselector?(s2, s1)
        end
      */

      if (selectors_equal(*pOne, *pTwo, true /*simpleSelectorOrderDependent*/)) {
        pOut = pOne;
        return true;
      }

      if (pOne->combinator() != Complex_Selector::ANCESTOR_OF || pTwo->combinator() != Complex_Selector::ANCESTOR_OF) {
        return false;
      }

      if (parentSuperselector(pOne, pTwo, mCtx)) {
        pOut = pTwo;
        return true;
      }

      if (parentSuperselector(pTwo, pOne, mCtx)) {
        pOut = pOne;
        return true;
      }

      return false;
    }
  };


  /*
  This is the equivalent of ruby's Sass::Util.lcs_backtrace.

  # Computes a single longest common subsequence for arrays x and y.
  # Algorithm from http://en.wikipedia.org/wiki/Longest_common_subsequence_problem#Reading_out_an_LCS
  */
  void lcs_backtrace(const LCSTable& c, ComplexSelectorDeque& x, ComplexSelectorDeque& y, int i, int j, const LcsCollectionComparator& comparator, ComplexSelectorDeque& out) {
    //DEBUG_PRINTLN(LCS, "LCSBACK: X=" << x << " Y=" << y << " I=" << i << " J=" << j)
    // TODO: make printComplexSelectorDeque and use DEBUG_EXEC AND DEBUG_PRINTLN HERE to get equivalent output

    if (i == 0 || j == 0) {
      DEBUG_PRINTLN(LCS, "RETURNING EMPTY")
      return;
    }


    Complex_Selector* pCompareOut = NULL;
    if (comparator(x[i], y[j], pCompareOut)) {
      DEBUG_PRINTLN(LCS, "RETURNING AFTER ELEM COMPARE")
      lcs_backtrace(c, x, y, i - 1, j - 1, comparator, out);
      out.push_back(pCompareOut);
      return;
    }

    if (c[i][j - 1] > c[i - 1][j]) {
      DEBUG_PRINTLN(LCS, "RETURNING AFTER TABLE COMPARE")
      lcs_backtrace(c, x, y, i, j - 1, comparator, out);
      return;
    }

    DEBUG_PRINTLN(LCS, "FINAL RETURN")
    lcs_backtrace(c, x, y, i - 1, j, comparator, out);
    return;
  }

  /*
  This is the equivalent of ruby's Sass::Util.lcs_table.

  # Calculates the memoization table for the Least Common Subsequence algorithm.
  # Algorithm from http://en.wikipedia.org/wiki/Longest_common_subsequence_problem#Computing_the_length_of_the_LCS
  */
  void lcs_table(const ComplexSelectorDeque& x, const ComplexSelectorDeque& y, const LcsCollectionComparator& comparator, LCSTable& out) {
    //DEBUG_PRINTLN(LCS, "LCSTABLE: X=" << x << " Y=" << y)
    // TODO: make printComplexSelectorDeque and use DEBUG_EXEC AND DEBUG_PRINTLN HERE to get equivalent output

    LCSTable c(x.size(), std::vector<int>(y.size()));

    // These shouldn't be necessary since the vector will be initialized to 0 already.
    // x.size.times {|i| c[i][0] = 0}
    // y.size.times {|j| c[0][j] = 0}

    for (size_t i = 1; i < x.size(); i++) {
      for (size_t j = 1; j < y.size(); j++) {
        Complex_Selector* pCompareOut = NULL;

        if (comparator(x[i], y[j], pCompareOut)) {
          c[i][j] = c[i - 1][j - 1] + 1;
        } else {
          c[i][j] = std::max(c[i][j - 1], c[i - 1][j]);
        }
      }
    }

    out = c;
  }

  /*
  This is the equivalent of ruby's Sass::Util.lcs.

  # Computes a single longest common subsequence for `x` and `y`.
  # If there are more than one longest common subsequences,
  # the one returned is that which starts first in `x`.

  # @param x [NodeCollection]
  # @param y [NodeCollection]
  # @comparator An equality check between elements of `x` and `y`.
  # @return [NodeCollection] The LCS

  http://en.wikipedia.org/wiki/Longest_common_subsequence_problem
  */
  void lcs(ComplexSelectorDeque& x, ComplexSelectorDeque& y, const LcsCollectionComparator& comparator, Context& ctx, ComplexSelectorDeque& out) {
    //DEBUG_PRINTLN(LCS, "LCS: X=" << x << " Y=" << y)
    // TODO: make printComplexSelectorDeque and use DEBUG_EXEC AND DEBUG_PRINTLN HERE to get equivalent output

    x.push_front(NULL);
    y.push_front(NULL);

    LCSTable table;
    lcs_table(x, y, comparator, table);

    return lcs_backtrace(table, x, y, static_cast<int>(x.size()) - 1, static_cast<int>(y.size()) - 1, comparator, out);
  }


  /*
   This is the equivalent of ruby's Sequence.trim.

   The following is the modified version of the ruby code that was more portable to C++. You
   should be able to drop it into ruby 3.2.19 and get the same results from ruby sass.

        # Avoid truly horrific quadratic behavior. TODO: I think there
        # may be a way to get perfect trimming without going quadratic.
        return seqses if seqses.size > 100

        # Keep the results in a separate array so we can be sure we aren't
        # comparing against an already-trimmed selector. This ensures that two
        # identical selectors don't mutually trim one another.
        result = seqses.dup

        # This is n^2 on the sequences, but only comparing between
        # separate sequences should limit the quadratic behavior.
        seqses.each_with_index do |seqs1, i|
          tempResult = []

          for seq1 in seqs1 do
            max_spec = 0
            for seq in _sources(seq1) do
              max_spec = [max_spec, seq.specificity].max
            end


            isMoreSpecificOuter = false
            for seqs2 in result do
              if seqs1.equal?(seqs2) then
                next
              end

              # Second Law of Extend: the specificity of a generated selector
              # should never be less than the specificity of the extending
              # selector.
              #
              # See https://github.com/nex3/sass/issues/324.
              isMoreSpecificInner = false
              for seq2 in seqs2 do
                isMoreSpecificInner = _specificity(seq2) >= max_spec && _superselector?(seq2, seq1)
                if isMoreSpecificInner then
                  break
                end
              end

              if isMoreSpecificInner then
                isMoreSpecificOuter = true
                break
              end
            end

            if !isMoreSpecificOuter then
              tempResult.push(seq1)
            end
          end

          result[i] = tempResult

        end

        result
   */
  /*
   - IMPROVEMENT: We could probably work directly in the output trimmed deque.
   */
  static Node trim(Node& seqses, Context& ctx, bool isReplace) {
    // See the comments in the above ruby code before embarking on understanding this function.

    // Avoid poor performance in extreme cases.
    if (seqses.collection()->size() > 100) {
      return seqses;
    }


    DEBUG_PRINTLN(TRIM, "TRIM: " << seqses)


    Node result = Node::createCollection();
    result.plus(seqses);

    DEBUG_PRINTLN(TRIM, "RESULT INITIAL: " << result)

    // Normally we use the standard STL iterators, but in this case, we need to access the result collection by index since we're
    // iterating the input collection, computing a value, and then setting the result in the output collection. We have to keep track
    // of the index manually.
    int toTrimIndex = 0;

    for (NodeDeque::iterator seqsesIter = seqses.collection()->begin(), seqsesIterEnd = seqses.collection()->end(); seqsesIter != seqsesIterEnd; ++seqsesIter) {
      Node& seqs1 = *seqsesIter;

      DEBUG_PRINTLN(TRIM, "SEQS1: " << seqs1 << " " << toTrimIndex)

      Node tempResult = Node::createCollection();
      tempResult.got_line_feed = seqs1.got_line_feed;

      for (NodeDeque::iterator seqs1Iter = seqs1.collection()->begin(), seqs1EndIter = seqs1.collection()->end(); seqs1Iter != seqs1EndIter; ++seqs1Iter) {
        Node& seq1 = *seqs1Iter;

        Complex_Selector* pSeq1 = nodeToComplexSelector(seq1, ctx);

        // Compute the maximum specificity. This requires looking at the "sources" of the sequence. See SimpleSequence.sources in the ruby code
        // for a good description of sources.
        //
        // TODO: I'm pretty sure there's a bug in the sources code. It was implemented for sass-spec's 182_test_nested_extend_loop test.
        // While the test passes, I compared the state of each trim call to verify correctness. The last trim call had incorrect sources. We
        // had an extra source that the ruby version did not have. Without a failing test case, this is going to be extra hard to find. My
        // best guess at this point is that we're cloning an object somewhere and maintaining the sources when we shouldn't be. This is purely
        // a guess though.
        unsigned long maxSpecificity = isReplace ? pSeq1->specificity() : 0;
        SourcesSet sources = pSeq1->sources();

        DEBUG_PRINTLN(TRIM, "TRIM SEQ1: " << seq1)
        DEBUG_EXEC(TRIM, printSourcesSet(sources, ctx, "TRIM SOURCES: "))

        for (SourcesSet::iterator sourcesSetIterator = sources.begin(), sourcesSetIteratorEnd = sources.end(); sourcesSetIterator != sourcesSetIteratorEnd; ++sourcesSetIterator) {
          const Complex_Selector* const pCurrentSelector = *sourcesSetIterator;
          maxSpecificity = std::max(maxSpecificity, pCurrentSelector->specificity());
        }

        DEBUG_PRINTLN(TRIM, "MAX SPECIFICITY: " << maxSpecificity)

        bool isMoreSpecificOuter = false;

        int resultIndex = 0;

        for (NodeDeque::iterator resultIter = result.collection()->begin(), resultIterEnd = result.collection()->end(); resultIter != resultIterEnd; ++resultIter) {
          Node& seqs2 = *resultIter;

          DEBUG_PRINTLN(TRIM, "SEQS1: " << seqs1)
          DEBUG_PRINTLN(TRIM, "SEQS2: " << seqs2)

          // Do not compare the same sequence to itself. The ruby call we're trying to
          // emulate is: seqs1.equal?(seqs2). equal? is an object comparison, not an equivalency comparision.
          // Since we have the same pointers in seqes and results, we can do a pointer comparision. seqs1 is
          // derived from seqses and seqs2 is derived from result.
          if (seqs1.collection() == seqs2.collection()) {
            DEBUG_PRINTLN(TRIM, "CONTINUE")
            continue;
          }

          bool isMoreSpecificInner = false;

          for (NodeDeque::iterator seqs2Iter = seqs2.collection()->begin(), seqs2IterEnd = seqs2.collection()->end(); seqs2Iter != seqs2IterEnd; ++seqs2Iter) {
            Node& seq2 = *seqs2Iter;

            Complex_Selector* pSeq2 = nodeToComplexSelector(seq2, ctx);

            DEBUG_PRINTLN(TRIM, "SEQ2 SPEC: " << pSeq2->specificity())
            DEBUG_PRINTLN(TRIM, "IS SPEC: " << pSeq2->specificity() << " >= " << maxSpecificity << " " << (pSeq2->specificity() >= maxSpecificity ? "true" : "false"))
            DEBUG_PRINTLN(TRIM, "IS SUPER: " << (pSeq2->is_superselector_of(pSeq1) ? "true" : "false"))

            isMoreSpecificInner = pSeq2->specificity() >= maxSpecificity && pSeq2->is_superselector_of(pSeq1);

            if (isMoreSpecificInner) {
              DEBUG_PRINTLN(TRIM, "FOUND MORE SPECIFIC")
              break;
            }
          }

          // If we found something more specific, we're done. Let the outer loop know and stop iterating.
          if (isMoreSpecificInner) {
            isMoreSpecificOuter = true;
            break;
          }

          resultIndex++;
        }

        if (!isMoreSpecificOuter) {
          DEBUG_PRINTLN(TRIM, "PUSHING: " << seq1)
          tempResult.collection()->push_back(seq1);
        }

      }

      DEBUG_PRINTLN(TRIM, "RESULT BEFORE ASSIGN: " << result)
      DEBUG_PRINTLN(TRIM, "TEMP RESULT: " << toTrimIndex << " " << tempResult)
      (*result.collection())[toTrimIndex] = tempResult;

      toTrimIndex++;

      DEBUG_PRINTLN(TRIM, "RESULT: " << result)
    }

    return result;
  }



  static bool parentSuperselector(const Node& one, const Node& two, Context& ctx) {
    // TODO: figure out a better way to create a Complex_Selector from scratch
    // TODO: There's got to be a better way. This got ugly quick...
    Position noPosition(-1, -1, -1);
    Type_Selector fakeParent(ParserState("[FAKE]"), "temp");
    Compound_Selector fakeHead(ParserState("[FAKE]"), 1 /*size*/);
    fakeHead.elements().push_back(&fakeParent);
    Complex_Selector fakeParentContainer(ParserState("[FAKE]"), Complex_Selector::ANCESTOR_OF, &fakeHead /*head*/, NULL /*tail*/);

    Complex_Selector* pOneWithFakeParent = nodeToComplexSelector(one, ctx);
    pOneWithFakeParent->set_innermost(&fakeParentContainer, Complex_Selector::ANCESTOR_OF);
    Complex_Selector* pTwoWithFakeParent = nodeToComplexSelector(two, ctx);
    pTwoWithFakeParent->set_innermost(&fakeParentContainer, Complex_Selector::ANCESTOR_OF);

    return pOneWithFakeParent->is_superselector_of(pTwoWithFakeParent);
  }


  class ParentSuperselectorChunker {
  public:
    ParentSuperselectorChunker(Node& lcs, Context& ctx) : mLcs(lcs), mCtx(ctx) {}
    Node& mLcs;
    Context& mCtx;

    bool operator()(const Node& seq) const {
      // {|s| parent_superselector?(s.first, lcs.first)}
      if (seq.collection()->size() == 0) return false;
      return parentSuperselector(seq.collection()->front(), mLcs.collection()->front(), mCtx);
    }
  };

  class SubweaveEmptyChunker {
  public:
    bool operator()(const Node& seq) const {
      // {|s| s.empty?}

      return seq.collection()->empty();
    }
  };

  /*
  # Takes initial subsequences of `seq1` and `seq2` and returns all
  # orderings of those subsequences. The initial subsequences are determined
  # by a block.
  #
  # Destructively removes the initial subsequences of `seq1` and `seq2`.
  #
  # For example, given `(A B C | D E)` and `(1 2 | 3 4 5)` (with `|`
  # denoting the boundary of the initial subsequence), this would return
  # `[(A B C 1 2), (1 2 A B C)]`. The sequences would then be `(D E)` and
  # `(3 4 5)`.
  #
  # @param seq1 [Array]
  # @param seq2 [Array]
  # @yield [a] Used to determine when to cut off the initial subsequences.
  #   Called repeatedly for each sequence until it returns true.
  # @yieldparam a [Array] A final subsequence of one input sequence after
  #   cutting off some initial subsequence.
  # @yieldreturn [Boolean] Whether or not to cut off the initial subsequence
  #   here.
  # @return [Array<Array>] All possible orderings of the initial subsequences.
  def chunks(seq1, seq2)
    chunk1 = []
    chunk1 << seq1.shift until yield seq1
    chunk2 = []
    chunk2 << seq2.shift until yield seq2
    return [] if chunk1.empty? && chunk2.empty?
    return [chunk2] if chunk1.empty?
    return [chunk1] if chunk2.empty?
    [chunk1 + chunk2, chunk2 + chunk1]
  end
  */
  template<typename ChunkerType>
  static Node chunks(Node& seq1, Node& seq2, const ChunkerType& chunker) {
    Node chunk1 = Node::createCollection();
    while (seq1.collection()->size() && !chunker(seq1)) {
      chunk1.collection()->push_back(seq1.collection()->front());
      seq1.collection()->pop_front();
    }

    Node chunk2 = Node::createCollection();
    while (!chunker(seq2)) {
      chunk2.collection()->push_back(seq2.collection()->front());
      seq2.collection()->pop_front();
    }

    if (chunk1.collection()->empty() && chunk2.collection()->empty()) {
      DEBUG_PRINTLN(CHUNKS, "RETURNING BOTH EMPTY")
      return Node::createCollection();
    }

    if (chunk1.collection()->empty()) {
      Node chunk2Wrapper = Node::createCollection();
      chunk2Wrapper.collection()->push_back(chunk2);
      DEBUG_PRINTLN(CHUNKS, "RETURNING ONE EMPTY")
      return chunk2Wrapper;
    }

    if (chunk2.collection()->empty()) {
      Node chunk1Wrapper = Node::createCollection();
      chunk1Wrapper.collection()->push_back(chunk1);
      DEBUG_PRINTLN(CHUNKS, "RETURNING TWO EMPTY")
      return chunk1Wrapper;
    }

    Node perms = Node::createCollection();

    Node firstPermutation = Node::createCollection();
    firstPermutation.collection()->insert(firstPermutation.collection()->end(), chunk1.collection()->begin(), chunk1.collection()->end());
    firstPermutation.collection()->insert(firstPermutation.collection()->end(), chunk2.collection()->begin(), chunk2.collection()->end());
    perms.collection()->push_back(firstPermutation);

    Node secondPermutation = Node::createCollection();
    secondPermutation.collection()->insert(secondPermutation.collection()->end(), chunk2.collection()->begin(), chunk2.collection()->end());
    secondPermutation.collection()->insert(secondPermutation.collection()->end(), chunk1.collection()->begin(), chunk1.collection()->end());
    perms.collection()->push_back(secondPermutation);

    DEBUG_PRINTLN(CHUNKS, "RETURNING PERM")

    return perms;
  }


  static Node groupSelectors(Node& seq, Context& ctx) {
    Node newSeq = Node::createCollection();

    Node tail = Node::createCollection();
    tail.plus(seq);

    while (!tail.collection()->empty()) {
      Node head = Node::createCollection();

      do {
        head.collection()->push_back(tail.collection()->front());
        tail.collection()->pop_front();
      } while (!tail.collection()->empty() && (head.collection()->back().isCombinator() || tail.collection()->front().isCombinator()));

      newSeq.collection()->push_back(head);
    }

    return newSeq;
  }


  static void getAndRemoveInitialOps(Node& seq, Node& ops) {
    NodeDeque& seqCollection = *(seq.collection());
    NodeDeque& opsCollection = *(ops.collection());

    while (seqCollection.size() > 0 && seqCollection.front().isCombinator()) {
      opsCollection.push_back(seqCollection.front());
      seqCollection.pop_front();
    }
  }


  static void getAndRemoveFinalOps(Node& seq, Node& ops) {
    NodeDeque& seqCollection = *(seq.collection());
    NodeDeque& opsCollection = *(ops.collection());

    while (seqCollection.size() > 0 && seqCollection.back().isCombinator()) {
      opsCollection.push_back(seqCollection.back()); // Purposefully reversed to match ruby code
      seqCollection.pop_back();
    }
  }


  /*
      def merge_initial_ops(seq1, seq2)
        ops1, ops2 = [], []
        ops1 << seq1.shift while seq1.first.is_a?(String)
        ops2 << seq2.shift while seq2.first.is_a?(String)

        newline = false
        newline ||= !!ops1.shift if ops1.first == "\n"
        newline ||= !!ops2.shift if ops2.first == "\n"

        # If neither sequence is a subsequence of the other, they cannot be
        # merged successfully
        lcs = Sass::Util.lcs(ops1, ops2)
        return unless lcs == ops1 || lcs == ops2
        return (newline ? ["\n"] : []) + (ops1.size > ops2.size ? ops1 : ops2)
      end
  */
  static Node mergeInitialOps(Node& seq1, Node& seq2, Context& ctx) {
    Node ops1 = Node::createCollection();
    Node ops2 = Node::createCollection();

    getAndRemoveInitialOps(seq1, ops1);
    getAndRemoveInitialOps(seq2, ops2);

    // TODO: Do we have this information available to us?
    // newline = false
    // newline ||= !!ops1.shift if ops1.first == "\n"
    // newline ||= !!ops2.shift if ops2.first == "\n"

    // If neither sequence is a subsequence of the other, they cannot be merged successfully
    DefaultLcsComparator lcsDefaultComparator;
    Node opsLcs = lcs(ops1, ops2, lcsDefaultComparator, ctx);

    if (!(opsLcs == ops1 || opsLcs == ops2)) {
      return Node::createNil();
    }

    // TODO: more newline logic
    // return (newline ? ["\n"] : []) + (ops1.size > ops2.size ? ops1 : ops2)

    return (ops1.collection()->size() > ops2.collection()->size() ? ops1 : ops2);
  }


  /*
      def merge_final_ops(seq1, seq2, res = [])


        # This code looks complicated, but it's actually just a bunch of special
        # cases for interactions between different combinators.
        op1, op2 = ops1.first, ops2.first
        if op1 && op2
          sel1 = seq1.pop
          sel2 = seq2.pop
          if op1 == '~' && op2 == '~'
            if sel1.superselector?(sel2)
              res.unshift sel2, '~'
            elsif sel2.superselector?(sel1)
              res.unshift sel1, '~'
            else
              merged = sel1.unify(sel2.members, sel2.subject?)
              res.unshift [
                [sel1, '~', sel2, '~'],
                [sel2, '~', sel1, '~'],
                ([merged, '~'] if merged)
              ].compact
            end
          elsif (op1 == '~' && op2 == '+') || (op1 == '+' && op2 == '~')
            if op1 == '~'
              tilde_sel, plus_sel = sel1, sel2
            else
              tilde_sel, plus_sel = sel2, sel1
            end

            if tilde_sel.superselector?(plus_sel)
              res.unshift plus_sel, '+'
            else
              merged = plus_sel.unify(tilde_sel.members, tilde_sel.subject?)
              res.unshift [
                [tilde_sel, '~', plus_sel, '+'],
                ([merged, '+'] if merged)
              ].compact
            end
          elsif op1 == '>' && %w[~ +].include?(op2)
            res.unshift sel2, op2
            seq1.push sel1, op1
          elsif op2 == '>' && %w[~ +].include?(op1)
            res.unshift sel1, op1
            seq2.push sel2, op2
          elsif op1 == op2
            return unless merged = sel1.unify(sel2.members, sel2.subject?)
            res.unshift merged, op1
          else
            # Unknown selector combinators can't be unified
            return
          end
          return merge_final_ops(seq1, seq2, res)
        elsif op1
          seq2.pop if op1 == '>' && seq2.last && seq2.last.superselector?(seq1.last)
          res.unshift seq1.pop, op1
          return merge_final_ops(seq1, seq2, res)
        else # op2
          seq1.pop if op2 == '>' && seq1.last && seq1.last.superselector?(seq2.last)
          res.unshift seq2.pop, op2
          return merge_final_ops(seq1, seq2, res)
        end
      end
  */
  static Node mergeFinalOps(Node& seq1, Node& seq2, Context& ctx, Node& res) {

    Node ops1 = Node::createCollection();
    Node ops2 = Node::createCollection();

    getAndRemoveFinalOps(seq1, ops1);
    getAndRemoveFinalOps(seq2, ops2);

    // TODO: do we have newlines to remove?
    // ops1.reject! {|o| o == "\n"}
    // ops2.reject! {|o| o == "\n"}

    if (ops1.collection()->empty() && ops2.collection()->empty()) {
      return res;
    }

    if (ops1.collection()->size() > 1 || ops2.collection()->size() > 1) {
      DefaultLcsComparator lcsDefaultComparator;
      Node opsLcs = lcs(ops1, ops2, lcsDefaultComparator, ctx);

      // If there are multiple operators, something hacky's going on. If one is a supersequence of the other, use that, otherwise give up.

      if (!(opsLcs == ops1 || opsLcs == ops2)) {
        return Node::createNil();
      }

      if (ops1.collection()->size() > ops2.collection()->size()) {
        res.collection()->insert(res.collection()->begin(), ops1.collection()->rbegin(), ops1.collection()->rend());
      } else {
        res.collection()->insert(res.collection()->begin(), ops2.collection()->rbegin(), ops2.collection()->rend());
      }

      return res;
    }

    if (!ops1.collection()->empty() && !ops2.collection()->empty()) {

      Node op1 = ops1.collection()->front();
      Node op2 = ops2.collection()->front();

      Node sel1 = seq1.collection()->back();
      seq1.collection()->pop_back();

      Node sel2 = seq2.collection()->back();
      seq2.collection()->pop_back();

      if (op1.combinator() == Complex_Selector::PRECEDES && op2.combinator() == Complex_Selector::PRECEDES) {

        if (sel1.selector()->is_superselector_of(sel2.selector())) {

          res.collection()->push_front(op1 /*PRECEDES - could have been op2 as well*/);
          res.collection()->push_front(sel2);

        } else if (sel2.selector()->is_superselector_of(sel1.selector())) {

          res.collection()->push_front(op1 /*PRECEDES - could have been op2 as well*/);
          res.collection()->push_front(sel1);

        } else {

          DEBUG_PRINTLN(ALL, "sel1: " << sel1)
          DEBUG_PRINTLN(ALL, "sel2: " << sel2)

          Complex_Selector* pMergedWrapper = sel1.selector()->clone(ctx); // Clone the Complex_Selector to get back to something we can transform to a node once we replace the head with the unification result
          // TODO: does subject matter? Ruby: return unless merged = sel1.unify(sel2.members, sel2.subject?)
          Compound_Selector* pMerged = sel1.selector()->head()->unify_with(sel2.selector()->head(), ctx);
          pMergedWrapper->head(pMerged);

          DEBUG_EXEC(ALL, printCompoundSelector(pMerged, "MERGED: "))

          Node newRes = Node::createCollection();

          Node firstPerm = Node::createCollection();
          firstPerm.collection()->push_back(sel1);
          firstPerm.collection()->push_back(Node::createCombinator(Complex_Selector::PRECEDES));
          firstPerm.collection()->push_back(sel2);
          firstPerm.collection()->push_back(Node::createCombinator(Complex_Selector::PRECEDES));
          newRes.collection()->push_back(firstPerm);

          Node secondPerm = Node::createCollection();
          secondPerm.collection()->push_back(sel2);
          secondPerm.collection()->push_back(Node::createCombinator(Complex_Selector::PRECEDES));
          secondPerm.collection()->push_back(sel1);
          secondPerm.collection()->push_back(Node::createCombinator(Complex_Selector::PRECEDES));
          newRes.collection()->push_back(secondPerm);

          if (pMerged) {
            Node mergedPerm = Node::createCollection();
            mergedPerm.collection()->push_back(Node::createSelector(pMergedWrapper, ctx));
            mergedPerm.collection()->push_back(Node::createCombinator(Complex_Selector::PRECEDES));
            newRes.collection()->push_back(mergedPerm);
          }

          res.collection()->push_front(newRes);

          DEBUG_PRINTLN(ALL, "RESULT: " << res)

        }

      } else if (((op1.combinator() == Complex_Selector::PRECEDES && op2.combinator() == Complex_Selector::ADJACENT_TO)) || ((op1.combinator() == Complex_Selector::ADJACENT_TO && op2.combinator() == Complex_Selector::PRECEDES))) {

          Node tildeSel = sel1;
          Node tildeOp = op1;
          Node plusSel = sel2;
          Node plusOp = op2;
          if (op1.combinator() != Complex_Selector::PRECEDES) {
            tildeSel = sel2;
            tildeOp = op2;
            plusSel = sel1;
            plusOp = op1;
          }

          if (tildeSel.selector()->is_superselector_of(plusSel.selector())) {

            res.collection()->push_front(plusOp);
            res.collection()->push_front(plusSel);

          } else {

            DEBUG_PRINTLN(ALL, "PLUS SEL: " << plusSel)
            DEBUG_PRINTLN(ALL, "TILDE SEL: " << tildeSel)

            Complex_Selector* pMergedWrapper = plusSel.selector()->clone(ctx); // Clone the Complex_Selector to get back to something we can transform to a node once we replace the head with the unification result
            // TODO: does subject matter? Ruby: merged = plus_sel.unify(tilde_sel.members, tilde_sel.subject?)
            Compound_Selector* pMerged = plusSel.selector()->head()->unify_with(tildeSel.selector()->head(), ctx);
            pMergedWrapper->head(pMerged);

            DEBUG_EXEC(ALL, printCompoundSelector(pMerged, "MERGED: "))

            Node newRes = Node::createCollection();

            Node firstPerm = Node::createCollection();
            firstPerm.collection()->push_back(tildeSel);
            firstPerm.collection()->push_back(Node::createCombinator(Complex_Selector::PRECEDES));
            firstPerm.collection()->push_back(plusSel);
            firstPerm.collection()->push_back(Node::createCombinator(Complex_Selector::ADJACENT_TO));
            newRes.collection()->push_back(firstPerm);

            if (pMerged) {
              Node mergedPerm = Node::createCollection();
              mergedPerm.collection()->push_back(Node::createSelector(pMergedWrapper, ctx));
              mergedPerm.collection()->push_back(Node::createCombinator(Complex_Selector::ADJACENT_TO));
              newRes.collection()->push_back(mergedPerm);
            }

            res.collection()->push_front(newRes);

            DEBUG_PRINTLN(ALL, "RESULT: " << res)

          }
      } else if (op1.combinator() == Complex_Selector::PARENT_OF && (op2.combinator() == Complex_Selector::PRECEDES || op2.combinator() == Complex_Selector::ADJACENT_TO)) {

        res.collection()->push_front(op2);
        res.collection()->push_front(sel2);

        seq1.collection()->push_back(sel1);
        seq1.collection()->push_back(op1);

      } else if (op2.combinator() == Complex_Selector::PARENT_OF && (op1.combinator() == Complex_Selector::PRECEDES || op1.combinator() == Complex_Selector::ADJACENT_TO)) {

        res.collection()->push_front(op1);
        res.collection()->push_front(sel1);

        seq2.collection()->push_back(sel2);
        seq2.collection()->push_back(op2);

      } else if (op1.combinator() == op2.combinator()) {

        DEBUG_PRINTLN(ALL, "sel1: " << sel1)
        DEBUG_PRINTLN(ALL, "sel2: " << sel2)

        Complex_Selector* pMergedWrapper = sel1.selector()->clone(ctx); // Clone the Complex_Selector to get back to something we can transform to a node once we replace the head with the unification result
        // TODO: does subject matter? Ruby: return unless merged = sel1.unify(sel2.members, sel2.subject?)
        Compound_Selector* pMerged = sel1.selector()->head()->unify_with(sel2.selector()->head(), ctx);
        pMergedWrapper->head(pMerged);

        DEBUG_EXEC(ALL, printCompoundSelector(pMerged, "MERGED: "))

        if (!pMerged) {
          return Node::createNil();
        }

        res.collection()->push_front(op1);
        res.collection()->push_front(Node::createSelector(pMergedWrapper, ctx));

        DEBUG_PRINTLN(ALL, "RESULT: " << res)

      } else {
        return Node::createNil();
      }

      return mergeFinalOps(seq1, seq2, ctx, res);

    } else if (!ops1.collection()->empty()) {

      Node op1 = ops1.collection()->front();

      if (op1.combinator() == Complex_Selector::PARENT_OF && !seq2.collection()->empty() && seq2.collection()->back().selector()->is_superselector_of(seq1.collection()->back().selector())) {
        seq2.collection()->pop_back();
      }

      // TODO: consider unshift(NodeCollection, Node)
      res.collection()->push_front(op1);
      res.collection()->push_front(seq1.collection()->back());
      seq1.collection()->pop_back();

      return mergeFinalOps(seq1, seq2, ctx, res);

    } else { // !ops2.collection()->empty()

      Node op2 = ops2.collection()->front();

      if (op2.combinator() == Complex_Selector::PARENT_OF && !seq1.collection()->empty() && seq1.collection()->back().selector()->is_superselector_of(seq2.collection()->back().selector())) {
        seq1.collection()->pop_back();
      }

      res.collection()->push_front(op2);
      res.collection()->push_front(seq2.collection()->back());
      seq2.collection()->pop_back();

      return mergeFinalOps(seq1, seq2, ctx, res);

    }

  }


  /*
    This is the equivalent of ruby's Sequence.subweave.

    Here is the original subweave code for reference during porting.

      def subweave(seq1, seq2)
        return [seq2] if seq1.empty?
        return [seq1] if seq2.empty?

        seq1, seq2 = seq1.dup, seq2.dup
        return unless init = merge_initial_ops(seq1, seq2)
        return unless fin = merge_final_ops(seq1, seq2)
        seq1 = group_selectors(seq1)
        seq2 = group_selectors(seq2)
        lcs = Sass::Util.lcs(seq2, seq1) do |s1, s2|
          next s1 if s1 == s2
          next unless s1.first.is_a?(SimpleSequence) && s2.first.is_a?(SimpleSequence)
          next s2 if parent_superselector?(s1, s2)
          next s1 if parent_superselector?(s2, s1)
        end

        diff = [[init]]
        until lcs.empty?
          diff << chunks(seq1, seq2) {|s| parent_superselector?(s.first, lcs.first)} << [lcs.shift]
          seq1.shift
          seq2.shift
        end
        diff << chunks(seq1, seq2) {|s| s.empty?}
        diff += fin.map {|sel| sel.is_a?(Array) ? sel : [sel]}
        diff.reject! {|c| c.empty?}

        result = Sass::Util.paths(diff).map {|p| p.flatten}.reject {|p| path_has_two_subjects?(p)}

        result
      end
  */
  Node Extend::subweave(Node& one, Node& two, Context& ctx) {
    // Check for the simple cases
    if (one.collection()->size() == 0) {
      Node out = Node::createCollection();
      out.collection()->push_back(two);
      return out;
    }
    if (two.collection()->size() == 0) {
      Node out = Node::createCollection();
      out.collection()->push_back(one);
      return out;
    }



    Node seq1 = Node::createCollection();
    seq1.plus(one);
    Node seq2 = Node::createCollection();
    seq2.plus(two);

    DEBUG_PRINTLN(SUBWEAVE, "SUBWEAVE ONE: " << seq1)
    DEBUG_PRINTLN(SUBWEAVE, "SUBWEAVE TWO: " << seq2)

    Node init = mergeInitialOps(seq1, seq2, ctx);
    if (init.isNil()) {
      return Node::createNil();
    }

    DEBUG_PRINTLN(SUBWEAVE, "INIT: " << init)

    Node res = Node::createCollection();
    Node fin = mergeFinalOps(seq1, seq2, ctx, res);
    if (fin.isNil()) {
      return Node::createNil();
    }

    DEBUG_PRINTLN(SUBWEAVE, "FIN: " << fin)


    // Moving this line up since fin isn't modified between now and when it happened before
    // fin.map {|sel| sel.is_a?(Array) ? sel : [sel]}

    for (NodeDeque::iterator finIter = fin.collection()->begin(), finEndIter = fin.collection()->end();
           finIter != finEndIter; ++finIter) {

      Node& childNode = *finIter;

      if (!childNode.isCollection()) {
        Node wrapper = Node::createCollection();
        wrapper.collection()->push_back(childNode);
        childNode = wrapper;
      }

    }

    DEBUG_PRINTLN(SUBWEAVE, "FIN MAPPED: " << fin)



    Node groupSeq1 = groupSelectors(seq1, ctx);
    DEBUG_PRINTLN(SUBWEAVE, "SEQ1: " << groupSeq1)

    Node groupSeq2 = groupSelectors(seq2, ctx);
    DEBUG_PRINTLN(SUBWEAVE, "SEQ2: " << groupSeq2)


    ComplexSelectorDeque groupSeq1Converted;
    nodeToComplexSelectorDeque(groupSeq1, groupSeq1Converted, ctx);

    ComplexSelectorDeque groupSeq2Converted;
    nodeToComplexSelectorDeque(groupSeq2, groupSeq2Converted, ctx);

    ComplexSelectorDeque out;
    LcsCollectionComparator collectionComparator(ctx);
    lcs(groupSeq2Converted, groupSeq1Converted, collectionComparator, ctx, out);
    Node seqLcs = complexSelectorDequeToNode(out, ctx);

    DEBUG_PRINTLN(SUBWEAVE, "SEQLCS: " << seqLcs)


    Node initWrapper = Node::createCollection();
    initWrapper.collection()->push_back(init);
    Node diff = Node::createCollection();
    diff.collection()->push_back(initWrapper);

    DEBUG_PRINTLN(SUBWEAVE, "DIFF INIT: " << diff)


    while (!seqLcs.collection()->empty()) {
      ParentSuperselectorChunker superselectorChunker(seqLcs, ctx);
      Node chunksResult = chunks(groupSeq1, groupSeq2, superselectorChunker);
      diff.collection()->push_back(chunksResult);

      Node lcsWrapper = Node::createCollection();
      lcsWrapper.collection()->push_back(seqLcs.collection()->front());
      seqLcs.collection()->pop_front();
      diff.collection()->push_back(lcsWrapper);

      if (groupSeq1.collection()->size()) groupSeq1.collection()->pop_front();
      if (groupSeq2.collection()->size()) groupSeq2.collection()->pop_front();
    }

    DEBUG_PRINTLN(SUBWEAVE, "DIFF POST LCS: " << diff)


    DEBUG_PRINTLN(SUBWEAVE, "CHUNKS: ONE=" << groupSeq1 << " TWO=" << groupSeq2)


    SubweaveEmptyChunker emptyChunker;
    Node chunksResult = chunks(groupSeq1, groupSeq2, emptyChunker);
    diff.collection()->push_back(chunksResult);


    DEBUG_PRINTLN(SUBWEAVE, "DIFF POST CHUNKS: " << diff)


    diff.collection()->insert(diff.collection()->end(), fin.collection()->begin(), fin.collection()->end());

    DEBUG_PRINTLN(SUBWEAVE, "DIFF POST FIN MAPPED: " << diff)

    // JMA - filter out the empty nodes (use a new collection, since iterator erase() invalidates the old collection)
    Node diffFiltered = Node::createCollection();
    for (NodeDeque::iterator diffIter = diff.collection()->begin(), diffEndIter = diff.collection()->end();
           diffIter != diffEndIter; ++diffIter) {
      Node& node = *diffIter;
      if (node.collection() && !node.collection()->empty()) {
        diffFiltered.collection()->push_back(node);
      }
    }
    diff = diffFiltered;

    DEBUG_PRINTLN(SUBWEAVE, "DIFF POST REJECT: " << diff)


    Node pathsResult = paths(diff, ctx);

    DEBUG_PRINTLN(SUBWEAVE, "PATHS: " << pathsResult)


    // We're flattening in place
    for (NodeDeque::iterator pathsIter = pathsResult.collection()->begin(), pathsEndIter = pathsResult.collection()->end();
      pathsIter != pathsEndIter; ++pathsIter) {

      Node& child = *pathsIter;
      child = flatten(child, ctx);
    }

    DEBUG_PRINTLN(SUBWEAVE, "FLATTENED: " << pathsResult)


    /*
      TODO: implement
      rejected = mapped.reject {|p| path_has_two_subjects?(p)}
      $stderr.puts "REJECTED: #{rejected}"
     */


    return pathsResult;

  }
  /*
  // disabled to avoid clang warning [-Wunused-function]
  static Node subweaveNaive(const Node& one, const Node& two, Context& ctx) {
    Node out = Node::createCollection();

    // Check for the simple cases
    if (one.isNil()) {
      out.collection()->push_back(two.clone(ctx));
    } else if (two.isNil()) {
      out.collection()->push_back(one.clone(ctx));
    } else {
      // Do the naive implementation. pOne = A B and pTwo = C D ...yields...  A B C D and C D A B
      // See https://gist.github.com/nex3/7609394 for details.

      Node firstPerm = one.clone(ctx);
      Node twoCloned = two.clone(ctx);
      firstPerm.plus(twoCloned);
      out.collection()->push_back(firstPerm);

      Node secondPerm = two.clone(ctx);
      Node oneCloned = one.clone(ctx);
      secondPerm.plus(oneCloned );
      out.collection()->push_back(secondPerm);
    }

    return out;
  }
  */


  /*
   This is the equivalent of ruby's Sequence.weave.

   The following is the modified version of the ruby code that was more portable to C++. You
   should be able to drop it into ruby 3.2.19 and get the same results from ruby sass.

      def weave(path)
        # This function works by moving through the selector path left-to-right,
        # building all possible prefixes simultaneously. These prefixes are
        # `befores`, while the remaining parenthesized suffixes is `afters`.
        befores = [[]]
        afters = path.dup

        until afters.empty?
          current = afters.shift.dup
          last_current = [current.pop]

          tempResult = []

          for before in befores do
            sub = subweave(before, current)
            if sub.nil?
              next
            end

            for seqs in sub do
              tempResult.push(seqs + last_current)
            end
          end

          befores = tempResult

        end

        return befores
      end
   */
  /*
      def weave(path)
        befores = [[]]
        afters = path.dup

        until afters.empty?
          current = afters.shift.dup

          last_current = [current.pop]


          tempResult = []

          for before in befores do
            sub = subweave(before, current)

            if sub.nil?
              next []
            end


            for seqs in sub do
              toPush = seqs + last_current

              tempResult.push(seqs + last_current)
            end

          end

          befores = tempResult

        end

        return befores
      end
  */
  static Node weave(Node& path, Context& ctx) {

    DEBUG_PRINTLN(WEAVE, "WEAVE: " << path)

    Node befores = Node::createCollection();
    befores.collection()->push_back(Node::createCollection());

    Node afters = Node::createCollection();
    afters.plus(path);

    while (!afters.collection()->empty()) {
      Node current = afters.collection()->front().clone(ctx);
      afters.collection()->pop_front();
      DEBUG_PRINTLN(WEAVE, "CURRENT: " << current)
      if (current.collection()->size() == 0) continue;

      Node last_current = Node::createCollection();
      last_current.collection()->push_back(current.collection()->back());
      current.collection()->pop_back();
      DEBUG_PRINTLN(WEAVE, "CURRENT POST POP: " << current)
      DEBUG_PRINTLN(WEAVE, "LAST CURRENT: " << last_current)

      Node tempResult = Node::createCollection();

      for (NodeDeque::iterator beforesIter = befores.collection()->begin(), beforesEndIter = befores.collection()->end(); beforesIter != beforesEndIter; beforesIter++) {
        Node& before = *beforesIter;

        Node sub = Extend::subweave(before, current, ctx);

        DEBUG_PRINTLN(WEAVE, "SUB: " << sub)

        if (sub.isNil()) {
          return Node::createCollection();
        }

        for (NodeDeque::iterator subIter = sub.collection()->begin(), subEndIter = sub.collection()->end(); subIter != subEndIter; subIter++) {
          Node& seqs = *subIter;

          Node toPush = Node::createCollection();
          toPush.plus(seqs);
          toPush.plus(last_current);

          tempResult.collection()->push_back(toPush);

        }
      }

      befores = tempResult;

    }

    return befores;
  }



  // This forward declaration is needed since extendComplexSelector calls extendCompoundSelector, which may recursively
  // call extendComplexSelector again.
  static Node extendComplexSelector(
    Complex_Selector* pComplexSelector,
    Context& ctx,
    ExtensionSubsetMap& subset_map,
    std::set<Compound_Selector> seen, bool isReplace, bool isOriginal);



  /*
   This is the equivalent of ruby's SimpleSequence.do_extend.

    // TODO: I think I have some modified ruby code to put here. Check.
  */
  /*
   ISSUES:
   - Previous TODO: Do we need to group the results by extender?
   - What does subject do in?: next unless unified = seq.members.last.unify(self_without_sel, subject?)
   - IMPROVEMENT: The search for uniqueness at the end is not ideal since it's has to loop over everything...
   - IMPROVEMENT: Check if the final search for uniqueness is doing anything that extendComplexSelector isn't already doing...
   */
  template<typename KeyType>
  class GroupByToAFunctor {
  public:
    KeyType operator()(ExtensionPair& extPair) const {
      Complex_Selector* pSelector = extPair.first;
      return *pSelector;
    }
  };
  static Node extendCompoundSelector(
    Compound_Selector* pSelector,
    Context& ctx,
    ExtensionSubsetMap& subset_map,
    std::set<Compound_Selector> seen, bool isReplace) {

    DEBUG_EXEC(EXTEND_COMPOUND, printCompoundSelector(pSelector, "EXTEND COMPOUND: "))
    // TODO: Ruby has another loop here to skip certain members?

    Node extendedSelectors = Node::createCollection();
    // extendedSelectors.got_line_feed = true;

    SubsetMapEntries entries = subset_map.get_v(pSelector->to_str_vec());

    typedef std::vector<std::pair<Complex_Selector, std::vector<ExtensionPair> > > GroupedByToAResult;

    GroupByToAFunctor<Complex_Selector> extPairKeyFunctor;
    GroupedByToAResult arr;
    group_by_to_a(entries, extPairKeyFunctor, arr);

    typedef std::pair<Compound_Selector*, Complex_Selector*> SelsNewSeqPair;
    typedef std::vector<SelsNewSeqPair> SelsNewSeqPairCollection;


    SelsNewSeqPairCollection holder;


    for (GroupedByToAResult::iterator groupedIter = arr.begin(), groupedIterEnd = arr.end(); groupedIter != groupedIterEnd; groupedIter++) {
      std::pair<Complex_Selector, std::vector<ExtensionPair> >& groupedPair = *groupedIter;

      Complex_Selector& seq = groupedPair.first;
      std::vector<ExtensionPair>& group = groupedPair.second;

      DEBUG_EXEC(EXTEND_COMPOUND, printComplexSelector(&seq, "SEQ: "))


      Compound_Selector* pSels = SASS_MEMORY_NEW(ctx.mem, Compound_Selector, pSelector->pstate());
      for (std::vector<ExtensionPair>::iterator groupIter = group.begin(), groupIterEnd = group.end(); groupIter != groupIterEnd; groupIter++) {
        ExtensionPair& pair = *groupIter;
        Compound_Selector* pCompound = pair.second;
        for (size_t index = 0; index < pCompound->length(); index++) {
          Simple_Selector* pSimpleSelector = (*pCompound)[index];
          (*pSels) << pSimpleSelector;
          pCompound->extended(true);
        }
      }

      DEBUG_EXEC(EXTEND_COMPOUND, printCompoundSelector(pSels, "SELS: "))

      Complex_Selector* pExtComplexSelector = &seq;    // The selector up to where the @extend is (ie, the thing to merge)
      Compound_Selector* pExtCompoundSelector = pSels; // All the simple selectors to be replaced from the current compound selector from all extensions

      // TODO: This can return a Compound_Selector with no elements. Should that just be returning NULL?
      // RUBY: self_without_sel = Sass::Util.array_minus(members, sels)
      Compound_Selector* pSelectorWithoutExtendSelectors = pSelector->minus(pExtCompoundSelector, ctx);

      DEBUG_EXEC(EXTEND_COMPOUND, printCompoundSelector(pSelector, "MEMBERS: "))
      DEBUG_EXEC(EXTEND_COMPOUND, printCompoundSelector(pSelectorWithoutExtendSelectors, "SELF_WO_SEL: "))

      Compound_Selector* pInnermostCompoundSelector = pExtComplexSelector->last()->head();
      Compound_Selector* pUnifiedSelector = NULL;

      if (!pInnermostCompoundSelector) {
        pInnermostCompoundSelector = SASS_MEMORY_NEW(ctx.mem, Compound_Selector, pSelector->pstate());
      }

      pUnifiedSelector = pInnermostCompoundSelector->unify_with(pSelectorWithoutExtendSelectors, ctx);


      DEBUG_EXEC(EXTEND_COMPOUND, printCompoundSelector(pInnermostCompoundSelector, "LHS: "))
      DEBUG_EXEC(EXTEND_COMPOUND, printCompoundSelector(pSelectorWithoutExtendSelectors, "RHS: "))
      DEBUG_EXEC(EXTEND_COMPOUND, printCompoundSelector(pUnifiedSelector, "UNIFIED: "))

      // RUBY: next unless unified
      if (!pUnifiedSelector || pUnifiedSelector->length() == 0) {
        continue;
      }

      // TODO: implement the parent directive match (if necessary based on test failures)
      // next if group.map {|e, _| check_directives_match!(e, parent_directives)}.none?

      // TODO: This seems a little fishy to me. See if it causes any problems. From the ruby, we should be able to just
      // get rid of the last Compound_Selector and replace it with this one. I think the reason this code is more
      // complex is that Complex_Selector contains a combinator, but in ruby combinators have already been filtered
      // out and aren't operated on.
      Complex_Selector* pNewSelector = pExtComplexSelector->cloneFully(ctx); // ->first();

      Complex_Selector* pNewInnerMost = SASS_MEMORY_NEW(ctx.mem, Complex_Selector, pSelector->pstate(), Complex_Selector::ANCESTOR_OF, pUnifiedSelector, NULL);

      Complex_Selector::Combinator combinator = pNewSelector->clear_innermost();
      pNewSelector->set_innermost(pNewInnerMost, combinator);

#ifdef DEBUG
      SourcesSet debugSet;
      debugSet = pNewSelector->sources();
      if (debugSet.size() > 0) {
        throw "The new selector should start with no sources. Something needs to be cloned to fix this.";
      }
      debugSet = pExtComplexSelector->sources();
      if (debugSet.size() > 0) {
        throw "The extension selector from our subset map should not have sources. These will bleed to the new selector. Something needs to be cloned to fix this.";
      }
#endif


      // if (pSelector && pSelector->has_line_feed()) pNewInnerMost->has_line_feed(true);
      // Set the sources on our new Complex_Selector to the sources of this simple sequence plus the thing we're extending.
      DEBUG_PRINTLN(EXTEND_COMPOUND, "SOURCES SETTING ON NEW SEQ: " << complexSelectorToNode(pNewSelector, ctx))

      DEBUG_EXEC(EXTEND_COMPOUND, SourcesSet oldSet = pNewSelector->sources(); printSourcesSet(oldSet, ctx, "SOURCES NEW SEQ BEGIN: "))

      SourcesSet newSourcesSet = pSelector->sources();
      DEBUG_EXEC(EXTEND_COMPOUND, printSourcesSet(newSourcesSet, ctx, "SOURCES THIS EXTEND: "))

      newSourcesSet.insert(pExtComplexSelector);
      DEBUG_EXEC(EXTEND_COMPOUND, printSourcesSet(newSourcesSet, ctx, "SOURCES WITH NEW SOURCE: "))

      // RUBY: new_seq.add_sources!(sources + [seq])
      pNewSelector->addSources(newSourcesSet, ctx);

      DEBUG_EXEC(EXTEND_COMPOUND, SourcesSet newSet = pNewSelector->sources(); printSourcesSet(newSet, ctx, "SOURCES ON NEW SELECTOR AFTER ADD: "))
      DEBUG_EXEC(EXTEND_COMPOUND, printSourcesSet(pSelector->sources(), ctx, "SOURCES THIS EXTEND WHICH SHOULD BE SAME STILL: "))


      if (pSels->has_line_feed()) pNewSelector->has_line_feed(true);;

      holder.push_back(std::make_pair(pSels, pNewSelector));
    }


    for (SelsNewSeqPairCollection::iterator holderIter = holder.begin(), holderIterEnd = holder.end(); holderIter != holderIterEnd; holderIter++) {
      SelsNewSeqPair& pair = *holderIter;

      Compound_Selector* pSels = pair.first;
      Complex_Selector* pNewSelector = pair.second;


      // RUBY??: next [] if seen.include?(sels)
      if (seen.find(*pSels) != seen.end()) {
        continue;
      }


      std::set<Compound_Selector> recurseSeen(seen);
      recurseSeen.insert(*pSels);


      DEBUG_PRINTLN(EXTEND_COMPOUND, "RECURSING DO EXTEND: " << complexSelectorToNode(pNewSelector, ctx))
      Node recurseExtendedSelectors = extendComplexSelector(pNewSelector, ctx, subset_map, recurseSeen, isReplace, false); // !:isOriginal

      DEBUG_PRINTLN(EXTEND_COMPOUND, "RECURSING DO EXTEND RETURN: " << recurseExtendedSelectors)

      for (NodeDeque::iterator iterator = recurseExtendedSelectors.collection()->begin(), endIterator = recurseExtendedSelectors.collection()->end();
           iterator != endIterator; ++iterator) {
        Node& newSelector = *iterator;

//        DEBUG_PRINTLN(EXTEND_COMPOUND, "EXTENDED AT THIS POINT: " << extendedSelectors)
//        DEBUG_PRINTLN(EXTEND_COMPOUND, "SELECTOR EXISTS ALREADY: " << newSelector << " " << extendedSelectors.contains(newSelector, false /*simpleSelectorOrderDependent*/));

        if (!extendedSelectors.contains(newSelector, false /*simpleSelectorOrderDependent*/)) {
//          DEBUG_PRINTLN(EXTEND_COMPOUND, "ADDING NEW SELECTOR")
          extendedSelectors.collection()->push_back(newSelector);
        }
      }
    }

    DEBUG_EXEC(EXTEND_COMPOUND, printCompoundSelector(pSelector, "EXTEND COMPOUND END: "))

    return extendedSelectors;
  }


  static bool complexSelectorHasExtension(
    Complex_Selector* pComplexSelector,
    Context& ctx,
    ExtensionSubsetMap& subset_map,
    std::set<Compound_Selector>& seen) {

    bool hasExtension = false;

    Complex_Selector* pIter = pComplexSelector;

    while (!hasExtension && pIter) {
      Compound_Selector* pHead = pIter->head();

      if (pHead) {
        if (seen.find(*pHead) == seen.end()) {
          for (Simple_Selector* pSimple : *pHead) {
            if (Wrapped_Selector* ws = dynamic_cast<Wrapped_Selector*>(pSimple)) {
              if (Selector_List* sl = dynamic_cast<Selector_List*>(ws->selector())) {
                for (Complex_Selector* cs : sl->elements()) {
                  while (cs) {
                    if (complexSelectorHasExtension(cs, ctx, subset_map, seen)) {
                      hasExtension = true;
                      break;
                    }
                    cs = cs->tail();
                  }
                }
              }
            }
          }
        }
        SubsetMapEntries entries = subset_map.get_v(pHead->to_str_vec());
        for (ExtensionPair ext : entries) {
          // check if both selectors have the same media block parent
          // if (ext.first->media_block() == pComplexSelector->media_block()) continue;
          if (ext.second->media_block() == 0) continue;
          if (pHead->media_block() &&
              ext.second->media_block()->media_queries() &&
              pHead->media_block()->media_queries()
          ) {
            std::string query_left(ext.second->media_block()->media_queries()->to_string(ctx.c_options));
            std::string query_right(pHead->media_block()->media_queries()->to_string(ctx.c_options));
            if (query_left == query_right) continue;
          }

          // fail if one goes across media block boundaries
          std::stringstream err;
          std::string cwd(Sass::File::get_cwd());
          ParserState pstate(ext.second->pstate());
          std::string rel_path(Sass::File::abs2rel(pstate.path, cwd, cwd));
          err << "You may not @extend an outer selector from within @media.\n";
          err << "You may only @extend selectors within the same directive.\n";
          err << "From \"@extend " << ext.second->to_string(ctx.c_options) << "\"";
          err << " on line " << pstate.line+1 << " of " << rel_path << "\n";
          error(err.str(), pComplexSelector->pstate());
        }
        if (entries.size() > 0) hasExtension = true;
      }

      pIter = pIter->tail();
    }

    return hasExtension;
  }


  /*
   This is the equivalent of ruby's Sequence.do_extend.

   // TODO: I think I have some modified ruby code to put here. Check.
   */
  /*
   ISSUES:
   - check to automatically include combinators doesn't transfer over to libsass' data model where
     the combinator and compound selector are one unit
     next [[sseq_or_op]] unless sseq_or_op.is_a?(SimpleSequence)
   */
  static Node extendComplexSelector(
    Complex_Selector* pComplexSelector,
    Context& ctx,
    ExtensionSubsetMap& subset_map,
    std::set<Compound_Selector> seen, bool isReplace, bool isOriginal) {

    Node complexSelector = complexSelectorToNode(pComplexSelector, ctx);
    DEBUG_PRINTLN(EXTEND_COMPLEX, "EXTEND COMPLEX: " << complexSelector)

    Node extendedNotExpanded = Node::createCollection();

    for (NodeDeque::iterator complexSelIter = complexSelector.collection()->begin(),
                             complexSelIterEnd = complexSelector.collection()->end();
         complexSelIter != complexSelIterEnd; ++complexSelIter)
    {

      Node& sseqOrOp = *complexSelIter;

      DEBUG_PRINTLN(EXTEND_COMPLEX, "LOOP: " << sseqOrOp)

      // If it's not a selector (meaning it's a combinator), just include it automatically
      // RUBY: next [[sseq_or_op]] unless sseq_or_op.is_a?(SimpleSequence)
      if (!sseqOrOp.isSelector()) {
        // Wrap our Combinator in two collections to match ruby. This is essentially making a collection Node
        // with one collection child. The collection child represents a Complex_Selector that is only a combinator.
        Node outer = Node::createCollection();
        Node inner = Node::createCollection();
        outer.collection()->push_back(inner);
        inner.collection()->push_back(sseqOrOp);
        extendedNotExpanded.collection()->push_back(outer);
        continue;
      }

      Compound_Selector* pCompoundSelector = sseqOrOp.selector()->head();

      // RUBY: extended = sseq_or_op.do_extend(extends, parent_directives, replace, seen)
      Node extended = extendCompoundSelector(pCompoundSelector, ctx, subset_map, seen, isReplace);
      if (sseqOrOp.got_line_feed) extended.got_line_feed = true;
      DEBUG_PRINTLN(EXTEND_COMPLEX, "EXTENDED: " << extended)


      // Prepend the Compound_Selector based on the choices logic; choices seems to be extend but with an ruby Array instead of a Sequence
      // due to the member mapping: choices = extended.map {|seq| seq.members}
      Complex_Selector* pJustCurrentCompoundSelector = sseqOrOp.selector();

      // RUBY: extended.first.add_sources!([self]) if original && !has_placeholder?
      if (isOriginal && !pComplexSelector->has_placeholder()) {
        SourcesSet srcset;
        srcset.insert(pComplexSelector);
        pJustCurrentCompoundSelector->addSources(srcset, ctx);
        DEBUG_PRINTLN(EXTEND_COMPLEX, "ADD SOURCES: " << *pComplexSelector)
      }

      bool isSuperselector = false;
      for (NodeDeque::iterator iterator = extended.collection()->begin(), endIterator = extended.collection()->end();
           iterator != endIterator; ++iterator) {
        Node& childNode = *iterator;
        Complex_Selector* pExtensionSelector = nodeToComplexSelector(childNode, ctx);
        if (pExtensionSelector->is_superselector_of(pJustCurrentCompoundSelector)) {
          isSuperselector = true;
          break;
        }
      }

      if (!isSuperselector) {
        if (sseqOrOp.got_line_feed) pJustCurrentCompoundSelector->has_line_feed(sseqOrOp.got_line_feed);
        extended.collection()->push_front(complexSelectorToNode(pJustCurrentCompoundSelector, ctx));
      }

      DEBUG_PRINTLN(EXTEND_COMPLEX, "CHOICES UNSHIFTED: " << extended)

      // Aggregate our current extensions
      extendedNotExpanded.collection()->push_back(extended);
    }


    DEBUG_PRINTLN(EXTEND_COMPLEX, "EXTENDED NOT EXPANDED: " << extendedNotExpanded)



    // Ruby Equivalent: paths
    Node paths = Sass::paths(extendedNotExpanded, ctx);

    DEBUG_PRINTLN(EXTEND_COMPLEX, "PATHS: " << paths)



    // Ruby Equivalent: weave
    Node weaves = Node::createCollection();

    for (NodeDeque::iterator pathsIter = paths.collection()->begin(), pathsEndIter = paths.collection()->end(); pathsIter != pathsEndIter; ++pathsIter) {
      Node& path = *pathsIter;
      Node weaved = weave(path, ctx);
      weaved.got_line_feed = path.got_line_feed;
      weaves.collection()->push_back(weaved);
    }

    DEBUG_PRINTLN(EXTEND_COMPLEX, "WEAVES: " << weaves)



    // Ruby Equivalent: trim
    Node trimmed = trim(weaves, ctx, isReplace);

    DEBUG_PRINTLN(EXTEND_COMPLEX, "TRIMMED: " << trimmed)


    // Ruby Equivalent: flatten
    Node extendedSelectors = flatten(trimmed, ctx, 1);

    DEBUG_PRINTLN(EXTEND_COMPLEX, ">>>>> EXTENDED: " << extendedSelectors)


    DEBUG_PRINTLN(EXTEND_COMPLEX, "EXTEND COMPLEX END: " << complexSelector)


    return extendedSelectors;
  }



  /*
   This is the equivalent of ruby's CommaSequence.do_extend.
  */
  Selector_List* Extend::extendSelectorList(Selector_List* pSelectorList, Context& ctx, ExtensionSubsetMap& subset_map, bool isReplace, bool& extendedSomething) {
    std::set<Compound_Selector> seen;
    return extendSelectorList(pSelectorList, ctx, subset_map, isReplace, extendedSomething, seen);
  }

  /*
   This is the equivalent of ruby's CommaSequence.do_extend.
  */
  Selector_List* Extend::extendSelectorList(Selector_List* pSelectorList, Context& ctx, ExtensionSubsetMap& subset_map, bool isReplace, bool& extendedSomething, std::set<Compound_Selector>& seen) {

    Selector_List* pNewSelectors = SASS_MEMORY_NEW(ctx.mem, Selector_List, pSelectorList->pstate(), pSelectorList->length());

    extendedSomething = false;

    for (size_t index = 0, length = pSelectorList->length(); index < length; index++) {
      Complex_Selector* pSelector = (*pSelectorList)[index];

      // ruby sass seems to keep a list of things that have extensions and then only extend those. We don't currently do that.
      // Since it's not that expensive to check if an extension exists in the subset map and since it can be relatively expensive to
      // run through the extend code (which does a data model transformation), check if there is anything to extend before doing
      // the extend. We might be able to optimize extendComplexSelector, but this approach keeps us closer to ruby sass (which helps
      // when debugging).
      if (!complexSelectorHasExtension(pSelector, ctx, subset_map, seen)) {
        *pNewSelectors << pSelector;
        continue;
      }

      extendedSomething = true;

      Node extendedSelectors = extendComplexSelector(pSelector, ctx, subset_map, seen, isReplace, true);
      if (!pSelector->has_placeholder()) {
        if (!extendedSelectors.contains(complexSelectorToNode(pSelector, ctx), true /*simpleSelectorOrderDependent*/)) {
          *pNewSelectors << pSelector;
          continue;
        }
      }

      for (NodeDeque::iterator iterator = extendedSelectors.collection()->begin(), iteratorBegin = extendedSelectors.collection()->begin(), iteratorEnd = extendedSelectors.collection()->end(); iterator != iteratorEnd; ++iterator) {
        // When it is a replace, skip the first one, unless there is only one
        if(isReplace && iterator == iteratorBegin && extendedSelectors.collection()->size() > 1 ) continue;

        Node& childNode = *iterator;
        *pNewSelectors << nodeToComplexSelector(childNode, ctx);
      }
    }

    Remove_Placeholders remove_placeholders(ctx);
    // it seems that we have to remove the place holders early here
    // normally we do this as the very last step (compare to ruby sass)
    pNewSelectors = remove_placeholders.remove_placeholders(pNewSelectors);

    // unwrap all wrapped selectors with inner lists
    for (Complex_Selector* cur : *pNewSelectors) {
      // process tails
      while (cur) {
        // process header
        if (cur->head() && seen.find(*cur->head()) == seen.end()) {
          std::set<Compound_Selector> recseen(seen);
          recseen.insert(*cur->head());
          // create a copy since we add multiple items if stuff get unwrapped
          Compound_Selector* cpy_head = SASS_MEMORY_NEW(ctx.mem, Compound_Selector, cur->pstate());
          for (Simple_Selector* hs : *cur->head()) {
            if (Wrapped_Selector* ws = dynamic_cast<Wrapped_Selector*>(hs)) {
              if (Selector_List* sl = dynamic_cast<Selector_List*>(ws->selector())) {
                // special case for ruby ass
                if (sl->empty()) {
                  // this seems inconsistent but it is how ruby sass seems to remove parentheses
                  *cpy_head << SASS_MEMORY_NEW(ctx.mem, Type_Selector, hs->pstate(), ws->name());
                }
                // has wrapped selectors
                else {
                  // extend the inner list of wrapped selector
                  Selector_List* ext_sl = extendSelectorList(sl, ctx, subset_map, recseen);
                  for (size_t i = 0; i < ext_sl->length(); i += 1) {
                    if (Complex_Selector* ext_cs = ext_sl->at(i)) {
                      // create clones for wrapped selector and the inner list
                      Wrapped_Selector* cpy_ws = SASS_MEMORY_NEW(ctx.mem, Wrapped_Selector, *ws);
                      Selector_List* cpy_ws_sl = SASS_MEMORY_NEW(ctx.mem, Selector_List, sl->pstate());
                      // remove parent selectors from inner selector
                      if (ext_cs->first()) {
                        if (ext_cs->first()->has_wrapped_selector()) {
                          continue; // ignore this case for now
                        }
                        *cpy_ws_sl << ext_cs->first();
                      }
                      // assign list to clone
                      cpy_ws->selector(cpy_ws_sl);
                      // append the clone
                      *cpy_head << cpy_ws;
                    }
                  }
                }
              } else {
                *cpy_head << hs;
              }
            } else {
              *cpy_head << hs;
            }
          }
          // replace header
          cur->head(cpy_head);
        }
        // process tail
        cur = cur->tail();
      }
    }
    return pNewSelectors;

  }


  bool shouldExtendBlock(Block* b) {

    // If a block is empty, there's no reason to extend it since any rules placed on this block
    // won't have any output. The main benefit of this is for structures like:
    //
    //    .a {
    //      .b {
    //        x: y;
    //      }
    //    }
    //
    // We end up visiting two rulesets (one with the selector .a and the other with the selector .a .b).
    // In this case, we don't want to try to pull rules onto .a since they won't get output anyway since
    // there are no child statements. However .a .b should have extensions applied.

    for (size_t i = 0, L = b->length(); i < L; ++i) {
      Statement* stm = (*b)[i];

      if (typeid(*stm) == typeid(Ruleset)) {
        // Do nothing. This doesn't count as a statement that causes extension since we'll iterate over this rule set in a future visit and try to extend it.
      }
      else {
        return true;
      }
    }

    return false;

  }


  // Extend a ruleset by extending the selectors and updating them on the ruleset. The block's rules don't need to change.
  template <typename ObjectType>
  static void extendObjectWithSelectorAndBlock(ObjectType* pObject, Context& ctx, ExtensionSubsetMap& subset_map) {

    DEBUG_PRINTLN(EXTEND_OBJECT, "FOUND SELECTOR: " << static_cast<Selector_List*>(pObject->selector())->to_string(ctx.c_options))

    // Ruby sass seems to filter nodes that don't have any content well before we get here. I'm not sure the repercussions
    // of doing so, so for now, let's just not extend things that won't be output later.
    if (!shouldExtendBlock(pObject->block())) {
      DEBUG_PRINTLN(EXTEND_OBJECT, "RETURNING WITHOUT EXTEND ATTEMPT")
      return;
    }

    bool extendedSomething = false;
    Selector_List* pNewSelectorList = Extend::extendSelectorList(static_cast<Selector_List*>(pObject->selector()), ctx, subset_map, false, extendedSomething);

    if (extendedSomething && pNewSelectorList) {
      DEBUG_PRINTLN(EXTEND_OBJECT, "EXTEND ORIGINAL SELECTORS: " << static_cast<Selector_List*>(pObject->selector())->to_string(ctx.c_options))
      DEBUG_PRINTLN(EXTEND_OBJECT, "EXTEND SETTING NEW SELECTORS: " << pNewSelectorList->to_string(ctx.c_options))
      pNewSelectorList->remove_parent_selectors();
      pObject->selector(pNewSelectorList);
    } else {
      DEBUG_PRINTLN(EXTEND_OBJECT, "EXTEND DID NOT TRY TO EXTEND ANYTHING")
    }
  }



  Extend::Extend(Context& ctx, ExtensionSubsetMap& ssm)
  : ctx(ctx), subset_map(ssm)
  { }

  void Extend::operator()(Block* b)
  {
    for (size_t i = 0, L = b->length(); i < L; ++i) {
      (*b)[i]->perform(this);
    }
    // do final check if everything was extended
    // we set `extended` flag on extended selectors
    if (b->is_root()) {
      // debug_subset_map(subset_map);
      for(auto const &it : subset_map.values()) {
        Complex_Selector* sel = it.first ? it.first->first() : NULL;
        Compound_Selector* ext = it.second ? it.second : NULL;
        if (ext && (ext->extended() || ext->is_optional())) continue;
        std::string str_sel(sel->to_string({ NESTED, 5 }));
        std::string str_ext(ext->to_string({ NESTED, 5 }));
        // debug_ast(sel, "sel: ");
        // debug_ast(ext, "ext: ");
        error("\"" + str_sel + "\" failed to @extend \"" + str_ext + "\".\n"
              "The selector \"" + str_ext + "\" was not found.\n"
              "Use \"@extend " + str_ext + " !optional\" if the"
                " extend should be able to fail.", ext->pstate());
      }
    }

  }

  void Extend::operator()(Ruleset* pRuleset)
  {
    extendObjectWithSelectorAndBlock(pRuleset, ctx, subset_map);
    pRuleset->block()->perform(this);
  }

  void Extend::operator()(Supports_Block* pFeatureBlock)
  {
    pFeatureBlock->block()->perform(this);
  }

  void Extend::operator()(Media_Block* pMediaBlock)
  {
    pMediaBlock->block()->perform(this);
  }

  void Extend::operator()(Directive* a)
  {
    // Selector_List* ls = dynamic_cast<Selector_List*>(a->selector());
    // selector_stack.push_back(ls);
    if (a->block()) a->block()->perform(this);
    // exp.selector_stack.pop_back();
  }
}
