/******************************************************************************
 ** Filename:    adaptmatch.c
 ** Purpose:     High level adaptive matcher.
 ** Author:      Dan Johnson
 ** History:     Mon Mar 11 10:00:10 1991, DSJ, Created.
 **
 ** (c) Copyright Hewlett-Packard Company, 1988.
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 ** http://www.apache.org/licenses/LICENSE-2.0
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 ******************************************************************************/

/*-----------------------------------------------------------------------------
          Include Files and Type Defines
-----------------------------------------------------------------------------*/
#include <ctype.h>
#include "ambigs.h"
#include "blobclass.h"
#include "blobs.h"
#include "helpers.h"
#include "normfeat.h"
#include "mfoutline.h"
#include "picofeat.h"
#include "float2int.h"
#include "outfeat.h"
#include "emalloc.h"
#include "intfx.h"
#include "speckle.h"
#include "efio.h"
#include "normmatch.h"
#include "permute.h"
#include "ndminx.h"
#include "intproto.h"
#include "const.h"
#include "globals.h"
#include "werd.h"
#include "callcpp.h"
#include "pageres.h"
#include "params.h"
#include "classify.h"
#include "shapetable.h"
#include "tessclassifier.h"
#include "trainingsample.h"
#include "unicharset.h"
#include "dict.h"
#include "featdefs.h"
#include "genericvector.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#ifdef __UNIX__
#include <assert.h>
#endif

// Include automatically generated configuration file if running autoconf.
#ifdef HAVE_CONFIG_H
#include "config_auto.h"
#endif

#define ADAPT_TEMPLATE_SUFFIX ".a"

#define MAX_MATCHES         10
#define UNLIKELY_NUM_FEAT 200
#define NO_DEBUG      0
#define MAX_ADAPTABLE_WERD_SIZE 40

#define ADAPTABLE_WERD_ADJUSTMENT    (0.05)

#define Y_DIM_OFFSET    (Y_SHIFT - BASELINE_Y_SHIFT)

#define WORST_POSSIBLE_RATING (1.0)

struct ScoredClass {
  CLASS_ID unichar_id;
  int shape_id;
  FLOAT32 rating;
  bool adapted;
  inT16 config;
  inT16 fontinfo_id;
  inT16 fontinfo_id2;
};

struct ADAPT_RESULTS {
  inT32 BlobLength;
  int NumMatches;
  bool HasNonfragment;
  ScoredClass match[MAX_NUM_CLASSES];
  ScoredClass best_match;
  CLASS_PRUNER_RESULTS CPResults;

  /// Initializes data members to the default values. Sets the initial
  /// rating of each class to be the worst possible rating (1.0).
  inline void Initialize() {
     BlobLength = MAX_INT32;
     NumMatches = 0;
     HasNonfragment = false;
     best_match.unichar_id = NO_CLASS;
     best_match.shape_id = -1;
     best_match.rating = WORST_POSSIBLE_RATING;
     best_match.adapted = false;
     best_match.config = 0;
     best_match.fontinfo_id = kBlankFontinfoId;
     best_match.fontinfo_id2 = kBlankFontinfoId;
  }
};

struct PROTO_KEY {
  ADAPT_TEMPLATES Templates;
  CLASS_ID ClassId;
  int ConfigId;
};

/*-----------------------------------------------------------------------------
          Private Macros
-----------------------------------------------------------------------------*/
#define MarginalMatch(Rating)       \
((Rating) > matcher_great_threshold)

#define InitIntFX() (FeaturesHaveBeenExtracted = FALSE)

/*-----------------------------------------------------------------------------
          Private Function Prototypes
-----------------------------------------------------------------------------*/
int CompareByRating(const void *arg1, const void *arg2);

ScoredClass *FindScoredUnichar(ADAPT_RESULTS *results, UNICHAR_ID id);

ScoredClass ScoredUnichar(ADAPT_RESULTS *results, UNICHAR_ID id);

void InitMatcherRatings(register FLOAT32 *Rating);

int MakeTempProtoPerm(void *item1, void *item2);

void SetAdaptiveThreshold(FLOAT32 Threshold);


/*-----------------------------------------------------------------------------
              Public Code
-----------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
namespace tesseract {
/**
 * This routine calls the adaptive matcher
 * which returns (in an array) the class id of each
 * class matched.
 *
 * It also returns the number of classes matched.
 * For each class matched it places the best rating
 * found for that class into the Ratings array.
 *
 * Bad matches are then removed so that they don't
 * need to be sorted.  The remaining good matches are
 * then sorted and converted to choices.
 *
 * This routine also performs some simple speckle
 * filtering.
 *
 * @note Exceptions: none
 * @note History: Mon Mar 11 10:00:58 1991, DSJ, Created.
 *
 * @param Blob    blob to be classified
 * @param denorm normalization/denormalization parameters
 * @param[out] Choices    List of choices found by adaptive matcher.
 * @param[out] CPResults  Array of CPResultStruct of size MAX_NUM_CLASSES is
 * filled on return with the choices found by the
 * class pruner and the ratings therefrom. Also
 * contains the detailed results of the integer matcher.
 *
 */
void Classify::AdaptiveClassifier(TBLOB *Blob,
                                  const DENORM& denorm,
                                  BLOB_CHOICE_LIST *Choices,
                                  CLASS_PRUNER_RESULTS CPResults) {
  assert(Choices != NULL);
  ADAPT_RESULTS *Results = new ADAPT_RESULTS();

  if (AdaptedTemplates == NULL)
    AdaptedTemplates = NewAdaptedTemplates (true);

  Results->Initialize();

  DoAdaptiveMatch(Blob, denorm, Results);
  if (CPResults != NULL)
    memcpy(CPResults, Results->CPResults,
           sizeof(CPResults[0]) * Results->NumMatches);

  RemoveBadMatches(Results);
  qsort((void *)Results->match, Results->NumMatches,
        sizeof(ScoredClass), CompareByRating);
  RemoveExtraPuncs(Results);
  ConvertMatchesToChoices(denorm, Blob->bounding_box(), Results, Choices);

  if (matcher_debug_level >= 1) {
    cprintf ("AD Matches =  ");
    PrintAdaptiveMatchResults(stdout, Results);
  }

  if (LargeSpeckle(Blob))
    AddLargeSpeckleTo(Choices);

#ifndef GRAPHICS_DISABLED
  if (classify_enable_adaptive_debugger)
    DebugAdaptiveClassifier(Blob, denorm, Results);
#endif

  NumClassesOutput += Choices->length();
  if (Choices->length() == 0) {
    if (!classify_bln_numeric_mode)
      tprintf ("Empty classification!\n");  // Should never normally happen.
    Choices = new BLOB_CHOICE_LIST();
    BLOB_CHOICE_IT temp_it;
    temp_it.set_to_list(Choices);
    temp_it.add_to_end(
        new BLOB_CHOICE(0, 50.0f, -20.0f, -1, -1, NULL, 0, 0, false));
  }

  delete Results;
}                                /* AdaptiveClassifier */

// If *win is NULL, sets it to a new ScrollView() object with title msg.
// Clears the window and draws baselines.
void Classify::RefreshDebugWindow(ScrollView **win, const char *msg,
                                  int y_offset, const TBOX &wbox) {
  #ifndef GRAPHICS_DISABLED
  const int kSampleSpaceWidth = 500;
  if (*win == NULL) {
    *win = new ScrollView(msg, 100, y_offset, kSampleSpaceWidth * 2, 200,
                          kSampleSpaceWidth * 2, 200, true);
  }
  (*win)->Clear();
  (*win)->Pen(64, 64, 64);
  (*win)->Line(-kSampleSpaceWidth, kBlnBaselineOffset,
               kSampleSpaceWidth, kBlnBaselineOffset);
  (*win)->Line(-kSampleSpaceWidth, kBlnXHeight + kBlnBaselineOffset,
               kSampleSpaceWidth, kBlnXHeight + kBlnBaselineOffset);
  (*win)->ZoomToRectangle(wbox.left(), wbox.top(),
                          wbox.right(), wbox.bottom());
  #endif  // GRAPHICS_DISABLED
}

// Learns the given word using its chopped_word, seam_array, denorm,
// box_word, best_state, and correct_text to learn both correctly and
// incorrectly segmented blobs. If filename is not NULL, then LearnBlob
// is called and the data will be written to a file for static training.
// Otherwise AdaptToBlob is called for adaption within a document.
// If rejmap is not NULL, then only chars with a rejmap entry of '1' will
// be learned, otherwise all chars with good correct_text are learned.
void Classify::LearnWord(const char* filename, const char *rejmap,
                         WERD_RES *word) {
  int word_len = word->correct_text.size();
  if (word_len == 0) return;

  float* thresholds = NULL;
  if (filename == NULL) {
    // Adaption mode.
    if (!EnableLearning || word->best_choice == NULL ||
        // If word->best_choice is not recorded at the top of accumulator's
        // best choices (which could happen for choices that are
        // altered with ReplaceAmbig()) we skip the adaption.
        !getDict().CurrentBestChoiceIs(*(word->best_choice)))
      return;  // Can't or won't adapt.

    NumWordsAdaptedTo++;
    if (classify_learning_debug_level >= 1)
      tprintf("\n\nAdapting to word = %s\n",
              word->best_choice->debug_string().string());
    thresholds = new float[word_len];
    GetAdaptThresholds(word->rebuild_word, word->denorm, *word->best_choice,
                       *word->raw_choice, thresholds);
  }
  int start_blob = 0;
  char prev_map_char = '0';

  #ifndef GRAPHICS_DISABLED
  if (classify_debug_character_fragments) {
    if (learn_fragmented_word_debug_win_ != NULL) {
      window_wait(learn_fragmented_word_debug_win_);
    }
    RefreshDebugWindow(&learn_fragments_debug_win_, "LearnPieces", 400,
                       word->chopped_word->bounding_box());
    RefreshDebugWindow(&learn_fragmented_word_debug_win_, "LearnWord", 200,
                       word->chopped_word->bounding_box());
    word->chopped_word->plot(learn_fragmented_word_debug_win_);
    ScrollView::Update();
  }
  #endif  // GRAPHICS_DISABLED

  for (int ch = 0; ch < word_len; ++ch) {
    if (classify_debug_character_fragments) {
      tprintf("\nLearning %s\n",  word->correct_text[ch].string());
    }
    char rej_map_char = rejmap != NULL ? *rejmap++ : '1';

    if (word->correct_text[ch].length() > 0 && rej_map_char == '1') {
      float threshold = thresholds != NULL ? thresholds[ch] : 0.0f;

      LearnPieces(filename, start_blob, word->best_state[ch],
                  threshold, CST_WHOLE, word->correct_text[ch].string(), word);

      if (word->best_state[ch] > 1 && !disable_character_fragments) {
        // Check that the character breaks into meaningful fragments
        // that each match a whole character with at least
        // classify_character_fragments_garbage_certainty_threshold
        bool garbage = false;
        TBLOB* frag_blob = word->chopped_word->blobs;
        for (int i = 0; i < start_blob; ++i) frag_blob = frag_blob->next;
        int frag;
        for (frag = 0; frag < word->best_state[ch]; ++frag) {
          if (classify_character_fragments_garbage_certainty_threshold < 0) {
            garbage |= LooksLikeGarbage(word->denorm, frag_blob);
          }
          frag_blob = frag_blob->next;
        }
        // Learn the fragments.
        if (!garbage) {
          bool pieces_all_natural = word->PiecesAllNatural(start_blob,
              word->best_state[ch]);
          if (pieces_all_natural || !prioritize_division) {
            for (frag = 0; frag < word->best_state[ch]; ++frag) {
              GenericVector<STRING> tokens;
              word->correct_text[ch].split(' ', &tokens);

              tokens[0] = CHAR_FRAGMENT::to_string(
                  tokens[0].string(), frag, word->best_state[ch],
                  pieces_all_natural);

              STRING full_string;
              for (int i = 0; i < tokens.size(); i++) {
                full_string += tokens[i];
                if (i != tokens.size() - 1)
                  full_string += ' ';
              }
              LearnPieces(filename, start_blob + frag, 1,
                          threshold, CST_FRAGMENT, full_string.string(), word);
            }
          }
        }
      }

      // TODO(rays): re-enable this part of the code when we switch to the
      // new classifier that needs to see examples of garbage.
      /*
      char next_map_char = ch + 1 < word_len
                           ? (rejmap != NULL ? *rejmap : '1')
                           : '0';
      if (word->best_state[ch] > 1) {
        // If the next blob is good, make junk with the rightmost fragment.
        if (ch + 1 < word_len && word->correct_text[ch + 1].length() > 0 &&
            next_map_char == '1') {
          LearnPieces(filename, start_blob + word->best_state[ch] - 1,
                      word->best_state[ch + 1] + 1,
                      threshold, CST_IMPROPER, INVALID_UNICHAR, word);
        }
        // If the previous blob is good, make junk with the leftmost fragment.
        if (ch > 0 && word->correct_text[ch - 1].length() > 0 &&
            prev_map_char == '1') {
          LearnPieces(filename, start_blob - word->best_state[ch - 1],
                      word->best_state[ch - 1] + 1,
                      threshold, CST_IMPROPER, INVALID_UNICHAR, word);
        }
      }
      // If the next blob is good, make a join with it.
      if (ch + 1 < word_len && word->correct_text[ch + 1].length() > 0 &&
          next_map_char == '1') {
        STRING joined_text = word->correct_text[ch];
        joined_text += word->correct_text[ch + 1];
        LearnPieces(filename, start_blob,
                    word->best_state[ch] + word->best_state[ch + 1],
                    threshold, CST_NGRAM, joined_text.string(), word);
      }
      */
    }
    start_blob += word->best_state[ch];
    prev_map_char = rej_map_char;
  }
  delete [] thresholds;
}  // LearnWord.

// Builds a blob of length fragments, from the word, starting at start,
// and then learns it, as having the given correct_text.
// If filename is not NULL, then LearnBlob
// is called and the data will be written to a file for static training.
// Otherwise AdaptToBlob is called for adaption within a document.
// threshold is a magic number required by AdaptToChar and generated by
// GetAdaptThresholds.
// Although it can be partly inferred from the string, segmentation is
// provided to explicitly clarify the character segmentation.
void Classify::LearnPieces(const char* filename, int start, int length,
                           float threshold, CharSegmentationType segmentation,
                           const char* correct_text, WERD_RES *word) {
  // TODO(daria) Remove/modify this if/when we want
  // to train and/or adapt to n-grams.
  if (segmentation != CST_WHOLE &&
      (segmentation != CST_FRAGMENT || disable_character_fragments))
    return;

  if (length > 1) {
    join_pieces(word->chopped_word->blobs, word->seam_array,
                start, start + length - 1);
  }
  TBLOB* blob = word->chopped_word->blobs;
  for (int i = 0; i < start; ++i)
    blob = blob->next;
  // Rotate the blob if needed for classification.
  const DENORM* denorm = &word->denorm;
  TBLOB* rotated_blob = blob->ClassifyNormalizeIfNeeded(&denorm);
  if (rotated_blob == NULL)
    rotated_blob = blob;

  #ifndef GRAPHICS_DISABLED
  // Draw debug windows showing the blob that is being learned if needed.
  if (strcmp(classify_learn_debug_str.string(), correct_text) == 0) {
    RefreshDebugWindow(&learn_debug_win_, "LearnPieces", 600,
                       word->chopped_word->bounding_box());
    rotated_blob->plot(learn_debug_win_, ScrollView::GREEN, ScrollView::BROWN);
    learn_debug_win_->Update();
    window_wait(learn_debug_win_);
  }
  if (classify_debug_character_fragments && segmentation == CST_FRAGMENT) {
    ASSERT_HOST(learn_fragments_debug_win_ != NULL);  // set up in LearnWord
    blob->plot(learn_fragments_debug_win_,
               ScrollView::BLUE, ScrollView::BROWN);
    learn_fragments_debug_win_->Update();
  }
  #endif  // GRAPHICS_DISABLED

  if (filename != NULL) {
    classify_norm_method.set_value(character);  // force char norm spc 30/11/93
    tess_bn_matching.set_value(false);    // turn it off
    tess_cn_matching.set_value(false);
    LearnBlob(feature_defs_, filename, rotated_blob, *denorm,
              correct_text);
  } else if (unicharset.contains_unichar(correct_text)) {
    UNICHAR_ID class_id = unicharset.unichar_to_id(correct_text);
    int font_id = word->fontinfo != NULL
                ? fontinfo_table_.get_id(*word->fontinfo)
                : 0;
    if (classify_learning_debug_level >= 1)
      tprintf("Adapting to char = %s, thr= %g font_id= %d\n",
              unicharset.id_to_unichar(class_id), threshold, font_id);
    // If filename is not NULL we are doing recognition
    // (as opposed to training), so we must have already set word fonts.
    AdaptToChar(rotated_blob, *denorm, class_id, font_id, threshold);
  } else if (classify_debug_level >= 1) {
    tprintf("Can't adapt to %s not in unicharset\n", correct_text);
  }
  if (rotated_blob != blob) {
    delete rotated_blob;
    delete denorm;
  }

  break_pieces(blob, word->seam_array, start, start + length - 1);
}  // LearnPieces.

/*---------------------------------------------------------------------------*/
/**
 * This routine performs cleanup operations
 * on the adaptive classifier.  It should be called
 * before the program is terminated.  Its main function
 * is to save the adapted templates to a file.
 *
 * Globals:
 * - #AdaptedTemplates current set of adapted templates
 * - #classify_save_adapted_templates TRUE if templates should be saved
 * - #classify_enable_adaptive_matcher TRUE if adaptive matcher is enabled
 *
 * @note Exceptions: none
 * @note History: Tue Mar 19 14:37:06 1991, DSJ, Created.
 */
void Classify::EndAdaptiveClassifier() {
  STRING Filename;
  FILE *File;

  #ifndef SECURE_NAMES
  if (AdaptedTemplates != NULL &&
      classify_enable_adaptive_matcher && classify_save_adapted_templates) {
    Filename = imagefile + ADAPT_TEMPLATE_SUFFIX;
    File = fopen (Filename.string(), "wb");
    if (File == NULL)
      cprintf ("Unable to save adapted templates to %s!\n", Filename.string());
    else {
      cprintf ("\nSaving adapted templates to %s ...", Filename.string());
      fflush(stdout);
      WriteAdaptedTemplates(File, AdaptedTemplates);
      cprintf ("\n");
      fclose(File);
    }
  }
  #endif

  if (AdaptedTemplates != NULL) {
    free_adapted_templates(AdaptedTemplates);
    AdaptedTemplates = NULL;
  }

  if (PreTrainedTemplates != NULL) {
    free_int_templates(PreTrainedTemplates);
    PreTrainedTemplates = NULL;
  }
  getDict().EndDangerousAmbigs();
  FreeNormProtos();
  if (AllProtosOn != NULL) {
    FreeBitVector(AllProtosOn);
    FreeBitVector(PrunedProtos);
    FreeBitVector(AllConfigsOn);
    FreeBitVector(AllProtosOff);
    FreeBitVector(AllConfigsOff);
    FreeBitVector(TempProtoMask);
    AllProtosOn = NULL;
    PrunedProtos = NULL;
    AllConfigsOn = NULL;
    AllProtosOff = NULL;
    AllConfigsOff = NULL;
    TempProtoMask = NULL;
  }
  delete shape_table_;
  shape_table_ = NULL;
}                                /* EndAdaptiveClassifier */


/*---------------------------------------------------------------------------*/
/**
 * This routine reads in the training
 * information needed by the adaptive classifier
 * and saves it into global variables.
 *  Parameters:
 *      load_pre_trained_templates  Indicates whether the pre-trained
 *                     templates (inttemp, normproto and pffmtable components)
 *                     should be lodaded. Should only be set to true if the
 *                     necesary classifier components are present in the
 *                     [lang].traineddata file.
 *  Globals:
 *      BuiltInTemplatesFile  file to get built-in temps from
 *      BuiltInCutoffsFile    file to get avg. feat per class from
 *      classify_use_pre_adapted_templates
 *                            enables use of pre-adapted templates
 *  @note History: Mon Mar 11 12:49:34 1991, DSJ, Created.
 */
void Classify::InitAdaptiveClassifier(bool load_pre_trained_templates) {
  if (!classify_enable_adaptive_matcher)
    return;
  if (AllProtosOn != NULL)
    EndAdaptiveClassifier();  // Don't leak with multiple inits.

  // If there is no language_data_path_prefix, the classifier will be
  // adaptive only.
  if (language_data_path_prefix.length() > 0 &&
      load_pre_trained_templates) {
    ASSERT_HOST(tessdata_manager.SeekToStart(TESSDATA_INTTEMP));
    PreTrainedTemplates =
      ReadIntTemplates(tessdata_manager.GetDataFilePtr());
    if (tessdata_manager.DebugLevel() > 0) tprintf("Loaded inttemp\n");

    if (tessdata_manager.SeekToStart(TESSDATA_SHAPE_TABLE)) {
      shape_table_ = new ShapeTable(unicharset);
      if (!shape_table_->DeSerialize(tessdata_manager.swap(),
                                     tessdata_manager.GetDataFilePtr())) {
        tprintf("Error loading shape table!\n");
        delete shape_table_;
        shape_table_ = NULL;
      } else if (tessdata_manager.DebugLevel() > 0) {
        tprintf("Successfully loaded shape table!\n");
      }
    }

    ASSERT_HOST(tessdata_manager.SeekToStart(TESSDATA_PFFMTABLE));
    ReadNewCutoffs(tessdata_manager.GetDataFilePtr(),
                   tessdata_manager.swap(),
                   tessdata_manager.GetEndOffset(TESSDATA_PFFMTABLE),
                   CharNormCutoffs);
    if (tessdata_manager.DebugLevel() > 0) tprintf("Loaded pffmtable\n");

    ASSERT_HOST(tessdata_manager.SeekToStart(TESSDATA_NORMPROTO));
    NormProtos =
      ReadNormProtos(tessdata_manager.GetDataFilePtr(),
                     tessdata_manager.GetEndOffset(TESSDATA_NORMPROTO));
    if (tessdata_manager.DebugLevel() > 0) tprintf("Loaded normproto\n");
  }

  im_.Init(&classify_debug_level, classify_integer_matcher_multiplier);
  InitIntegerFX();

  AllProtosOn = NewBitVector(MAX_NUM_PROTOS);
  PrunedProtos = NewBitVector(MAX_NUM_PROTOS);
  AllConfigsOn = NewBitVector(MAX_NUM_CONFIGS);
  AllProtosOff = NewBitVector(MAX_NUM_PROTOS);
  AllConfigsOff = NewBitVector(MAX_NUM_CONFIGS);
  TempProtoMask = NewBitVector(MAX_NUM_PROTOS);
  set_all_bits(AllProtosOn, WordsInVectorOfSize(MAX_NUM_PROTOS));
  set_all_bits(PrunedProtos, WordsInVectorOfSize(MAX_NUM_PROTOS));
  set_all_bits(AllConfigsOn, WordsInVectorOfSize(MAX_NUM_CONFIGS));
  zero_all_bits(AllProtosOff, WordsInVectorOfSize(MAX_NUM_PROTOS));
  zero_all_bits(AllConfigsOff, WordsInVectorOfSize(MAX_NUM_CONFIGS));

  for (int i = 0; i < MAX_NUM_CLASSES; i++) {
     BaselineCutoffs[i] = 0;
  }

  if (classify_use_pre_adapted_templates) {
    FILE *File;
    STRING Filename;

    Filename = imagefile;
    Filename += ADAPT_TEMPLATE_SUFFIX;
    File = fopen(Filename.string(), "rb");
    if (File == NULL) {
      AdaptedTemplates = NewAdaptedTemplates(true);
    } else {
      #ifndef SECURE_NAMES
      cprintf("\nReading pre-adapted templates from %s ...\n",
              Filename.string());
      fflush(stdout);
      #endif
      AdaptedTemplates = ReadAdaptedTemplates(File);
      cprintf("\n");
      fclose(File);
      PrintAdaptedTemplates(stdout, AdaptedTemplates);

      for (int i = 0; i < AdaptedTemplates->Templates->NumClasses; i++) {
        BaselineCutoffs[i] = CharNormCutoffs[i];
      }
    }
  } else {
    if (AdaptedTemplates != NULL)
      free_adapted_templates(AdaptedTemplates);
    AdaptedTemplates = NewAdaptedTemplates(true);
  }
}                                /* InitAdaptiveClassifier */

void Classify::ResetAdaptiveClassifierInternal() {
  if (classify_learning_debug_level > 0) {
    tprintf("Resetting adaptive classifier (NumAdaptationsFailed=%d)\n",
            NumAdaptationsFailed);
  }
  free_adapted_templates(AdaptedTemplates);
  AdaptedTemplates = NULL;
  NumAdaptationsFailed = 0;
}


/*---------------------------------------------------------------------------*/
/**
 * Print to File the statistics which have
 * been gathered for the adaptive matcher.
 *
 * @param File open text file to print adaptive statistics to
 *
 * Globals: none
 *
 * @note Exceptions: none
 * @note History: Thu Apr 18 14:37:37 1991, DSJ, Created.
 */
void Classify::PrintAdaptiveStatistics(FILE *File) {
  #ifndef SECURE_NAMES

  fprintf (File, "\nADAPTIVE MATCHER STATISTICS:\n");
  fprintf (File, "\tNum blobs classified = %d\n", AdaptiveMatcherCalls);
  fprintf (File, "\tNum classes output   = %d (Avg = %4.2f)\n",
    NumClassesOutput,
    ((AdaptiveMatcherCalls == 0) ? (0.0) :
  ((float) NumClassesOutput / AdaptiveMatcherCalls)));
  fprintf (File, "\t\tBaseline Classifier: %4d calls (%4.2f classes/call)\n",
    BaselineClassifierCalls,
    ((BaselineClassifierCalls == 0) ? (0.0) :
  ((float) NumBaselineClassesTried / BaselineClassifierCalls)));
  fprintf (File, "\t\tCharNorm Classifier: %4d calls (%4.2f classes/call)\n",
    CharNormClassifierCalls,
    ((CharNormClassifierCalls == 0) ? (0.0) :
  ((float) NumCharNormClassesTried / CharNormClassifierCalls)));
  fprintf (File, "\t\tAmbig    Classifier: %4d calls (%4.2f classes/call)\n",
    AmbigClassifierCalls,
    ((AmbigClassifierCalls == 0) ? (0.0) :
  ((float) NumAmbigClassesTried / AmbigClassifierCalls)));

  fprintf (File, "\nADAPTIVE LEARNER STATISTICS:\n");
  fprintf (File, "\tNumber of words adapted to: %d\n", NumWordsAdaptedTo);
  fprintf (File, "\tNumber of chars adapted to: %d\n", NumCharsAdaptedTo);

  PrintAdaptedTemplates(File, AdaptedTemplates);
  #endif
}                                /* PrintAdaptiveStatistics */


/*---------------------------------------------------------------------------*/
/**
 * This routine prepares the adaptive
 * matcher for the start
 * of the first pass.  Learning is enabled (unless it
 * is disabled for the whole program).
 *
 * @note this is somewhat redundant, it simply says that if learning is
 * enabled then it will remain enabled on the first pass.  If it is
 * disabled, then it will remain disabled.  This is only put here to
 * make it very clear that learning is controlled directly by the global
 * setting of EnableLearning.
 *
 * Globals:
 * - #EnableLearning
 * set to TRUE by this routine
 *
 * @note Exceptions: none
 * @note History: Mon Apr 15 16:39:29 1991, DSJ, Created.
 */
void Classify::SettupPass1() {
  EnableLearning = classify_enable_learning;

  getDict().SettupStopperPass1();

}                                /* SettupPass1 */


/*---------------------------------------------------------------------------*/
/**
 * This routine prepares the adaptive
 * matcher for the start of the second pass.  Further
 * learning is disabled.
 *
 * Globals:
 * - #EnableLearning set to FALSE by this routine
 *
 * @note Exceptions: none
 * @note History: Mon Apr 15 16:39:29 1991, DSJ, Created.
 */
void Classify::SettupPass2() {
  EnableLearning = FALSE;
  getDict().SettupStopperPass2();

}                                /* SettupPass2 */


/*---------------------------------------------------------------------------*/
/**
 * This routine creates a new adapted
 * class and uses Blob as the model for the first
 * config in that class.
 *
 * @param Blob blob to model new class after
 * @param denorm normalization/denormalization parameters
 * @param ClassId id of the class to be initialized
 * @param FontinfoId font information inferred from pre-trained templates
 * @param Class adapted class to be initialized
 * @param Templates adapted templates to add new class to
 *
 * Globals:
 * - #AllProtosOn dummy mask with all 1's
 * - BaselineCutoffs kludge needed to get cutoffs
 * - #PreTrainedTemplates kludge needed to get cutoffs
 *
 * @note Exceptions: none
 * @note History: Thu Mar 14 12:49:39 1991, DSJ, Created.
 */
void Classify::InitAdaptedClass(TBLOB *Blob,
                                const DENORM& denorm,
                                CLASS_ID ClassId,
                                int FontinfoId,
                                ADAPT_CLASS Class,
                                ADAPT_TEMPLATES Templates) {
  FEATURE_SET Features;
  int Fid, Pid;
  FEATURE Feature;
  int NumFeatures;
  TEMP_PROTO TempProto;
  PROTO Proto;
  INT_CLASS IClass;
  TEMP_CONFIG Config;

  classify_norm_method.set_value(baseline);
  Features = ExtractOutlineFeatures(Blob);
  NumFeatures = Features->NumFeatures;
  if (NumFeatures > UNLIKELY_NUM_FEAT || NumFeatures <= 0) {
    FreeFeatureSet(Features);
    return;
  }

  Config = NewTempConfig(NumFeatures - 1, FontinfoId);
  TempConfigFor(Class, 0) = Config;

  /* this is a kludge to construct cutoffs for adapted templates */
  if (Templates == AdaptedTemplates)
    BaselineCutoffs[ClassId] = CharNormCutoffs[ClassId];

  IClass = ClassForClassId (Templates->Templates, ClassId);

  for (Fid = 0; Fid < Features->NumFeatures; Fid++) {
    Pid = AddIntProto (IClass);
    assert (Pid != NO_PROTO);

    Feature = Features->Features[Fid];
    TempProto = NewTempProto ();
    Proto = &(TempProto->Proto);

    /* compute proto params - NOTE that Y_DIM_OFFSET must be used because
       ConvertProto assumes that the Y dimension varies from -0.5 to 0.5
       instead of the -0.25 to 0.75 used in baseline normalization */
    Proto->Angle = Feature->Params[OutlineFeatDir];
    Proto->X = Feature->Params[OutlineFeatX];
    Proto->Y = Feature->Params[OutlineFeatY] - Y_DIM_OFFSET;
    Proto->Length = Feature->Params[OutlineFeatLength];
    FillABC(Proto);

    TempProto->ProtoId = Pid;
    SET_BIT (Config->Protos, Pid);

    ConvertProto(Proto, Pid, IClass);
    AddProtoToProtoPruner(Proto, Pid, IClass,
                          classify_learning_debug_level >= 2);

    Class->TempProtos = push (Class->TempProtos, TempProto);
  }
  FreeFeatureSet(Features);

  AddIntConfig(IClass);
  ConvertConfig (AllProtosOn, 0, IClass);

  if (classify_learning_debug_level >= 1) {
    cprintf ("Added new class '%s' with class id %d and %d protos.\n",
             unicharset.id_to_unichar(ClassId), ClassId, NumFeatures);
    if (classify_learning_debug_level > 1)
      DisplayAdaptedChar(Blob, denorm, IClass);
  }

  if (IsEmptyAdaptedClass(Class))
    (Templates->NumNonEmptyClasses)++;
}                                /* InitAdaptedClass */


/*---------------------------------------------------------------------------*/
/**
 * This routine sets up the feature
 * extractor to extract baseline normalized
 * pico-features.
 *
 * The extracted pico-features are converted
 * to integer form and placed in IntFeatures. The
 * original floating-pt. features are returned in
 * FloatFeatures.
 *
 * Globals: none
 * @param Blob blob to extract features from
 * @param[out] IntFeatures array to fill with integer features
 * @param[out] FloatFeatures place to return actual floating-pt features
 *
 * @return Number of pico-features returned (0 if
 * an error occurred)
 * @note Exceptions: none
 * @note History: Tue Mar 12 17:55:18 1991, DSJ, Created.
 */
int Classify::GetAdaptiveFeatures(TBLOB *Blob,
                                  INT_FEATURE_ARRAY IntFeatures,
                                  FEATURE_SET *FloatFeatures) {
  FEATURE_SET Features;
  int NumFeatures;

  classify_norm_method.set_value(baseline);
  Features = ExtractPicoFeatures(Blob);

  NumFeatures = Features->NumFeatures;
  if (NumFeatures > UNLIKELY_NUM_FEAT) {
    FreeFeatureSet(Features);
    return 0;
  }

  ComputeIntFeatures(Features, IntFeatures);
  *FloatFeatures = Features;

  return NumFeatures;
}                                /* GetAdaptiveFeatures */


/*-----------------------------------------------------------------------------
              Private Code
-----------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
/**
 * Return TRUE if the specified word is
 * acceptable for adaptation.
 *
 * Globals: none
 *
 * @param Word current word
 * @param BestChoiceWord best overall choice for word with context
 * @param RawChoiceWord best choice for word without context
 *
 * @return TRUE or FALSE
 * @note Exceptions: none
 * @note History: Thu May 30 14:25:06 1991, DSJ, Created.
 */
int Classify::AdaptableWord(TWERD *Word,
                            const WERD_CHOICE &BestChoiceWord,
                            const WERD_CHOICE &RawChoiceWord) {
  int BestChoiceLength = BestChoiceWord.length();
  float adaptable_score =
    getDict().segment_penalty_dict_case_ok + ADAPTABLE_WERD_ADJUSTMENT;
  return   // rules that apply in general - simplest to compute first
      BestChoiceLength > 0 &&
      BestChoiceLength == Word->NumBlobs() &&
      BestChoiceLength <= MAX_ADAPTABLE_WERD_SIZE &&
      getDict().CurrentBestChoiceAdjustFactor() <= adaptable_score &&
      getDict().AlternativeChoicesWorseThan(adaptable_score) &&
      getDict().CurrentBestChoiceIs(BestChoiceWord);
}

/*---------------------------------------------------------------------------*/
/**
 * @param Blob blob to add to templates for ClassId
 * @param denorm normalization/denormalization parameters
 * @param ClassId class to add blob to
 * @param FontinfoId font information from pre-trained templates
 * @param Threshold minimum match rating to existing template
 *
 * Globals:
 * - AdaptedTemplates current set of adapted templates
 * - AllProtosOn dummy mask to match against all protos
 * - AllConfigsOn dummy mask to match against all configs
 *
 * @return none
 * @note Exceptions: none
 * @note History: Thu Mar 14 09:36:03 1991, DSJ, Created.
 */
void Classify::AdaptToChar(TBLOB *Blob,
                           const DENORM& denorm,
                           CLASS_ID ClassId,
                           int FontinfoId,
                           FLOAT32 Threshold) {
  int NumFeatures;
  INT_FEATURE_ARRAY IntFeatures;
  INT_RESULT_STRUCT IntResult;
  INT_CLASS IClass;
  ADAPT_CLASS Class;
  TEMP_CONFIG TempConfig;
  FEATURE_SET FloatFeatures;
  int NewTempConfigId;

  ResetFeaturesHaveBeenExtracted();
  NumCharsAdaptedTo++;
  if (!LegalClassId (ClassId))
    return;

  Class = AdaptedTemplates->Class[ClassId];
  assert(Class != NULL);
  if (IsEmptyAdaptedClass(Class)) {
    InitAdaptedClass(Blob, denorm, ClassId, FontinfoId, Class,
                     AdaptedTemplates);
  }
  else {
    IClass = ClassForClassId (AdaptedTemplates->Templates, ClassId);

    NumFeatures = GetAdaptiveFeatures(Blob, IntFeatures, &FloatFeatures);
    if (NumFeatures <= 0)
      return;

    im_.SetBaseLineMatch();
    // Only match configs with the matching font.
    BIT_VECTOR MatchingFontConfigs = NewBitVector(MAX_NUM_PROTOS);
    for (int cfg = 0; cfg < IClass->NumConfigs; ++cfg) {
      if (GetFontinfoId(Class, cfg) == FontinfoId) {
        SET_BIT(MatchingFontConfigs, cfg);
      } else {
        reset_bit(MatchingFontConfigs, cfg);
      }
    }
    im_.Match(IClass, AllProtosOn, MatchingFontConfigs,
              NumFeatures, IntFeatures,
              &IntResult, classify_adapt_feature_threshold,
              NO_DEBUG, matcher_debug_separate_windows);
    FreeBitVector(MatchingFontConfigs);

    SetAdaptiveThreshold(Threshold);

    if (IntResult.Rating <= Threshold) {
      if (ConfigIsPermanent (Class, IntResult.Config)) {
        if (classify_learning_debug_level >= 1)
          cprintf ("Found good match to perm config %d = %4.1f%%.\n",
            IntResult.Config, (1.0 - IntResult.Rating) * 100.0);
        FreeFeatureSet(FloatFeatures);
        return;
      }

      TempConfig = TempConfigFor (Class, IntResult.Config);
      IncreaseConfidence(TempConfig);
      if (TempConfig->NumTimesSeen > Class->MaxNumTimesSeen) {
        Class->MaxNumTimesSeen = TempConfig->NumTimesSeen;
      }
      if (classify_learning_debug_level >= 1)
        cprintf ("Increasing reliability of temp config %d to %d.\n",
          IntResult.Config, TempConfig->NumTimesSeen);

      if (TempConfigReliable(ClassId, TempConfig)) {
        MakePermanent(AdaptedTemplates, ClassId, IntResult.Config, denorm,
                      Blob);
        UpdateAmbigsGroup(ClassId, denorm, Blob);
      }
    }
    else {
      if (classify_learning_debug_level >= 1) {
        cprintf ("Found poor match to temp config %d = %4.1f%%.\n",
          IntResult.Config, (1.0 - IntResult.Rating) * 100.0);
        if (classify_learning_debug_level > 2)
          DisplayAdaptedChar(Blob, denorm, IClass);
      }
      NewTempConfigId = MakeNewTemporaryConfig(AdaptedTemplates,
                                               ClassId,
                                               FontinfoId,
                                               NumFeatures,
                                               IntFeatures,
                                               FloatFeatures);
      if (NewTempConfigId >= 0 &&
          TempConfigReliable(ClassId, TempConfigFor(Class, NewTempConfigId))) {
        MakePermanent(AdaptedTemplates, ClassId, NewTempConfigId, denorm, Blob);
        UpdateAmbigsGroup(ClassId, denorm, Blob);
      }

#ifndef GRAPHICS_DISABLED
      if (classify_learning_debug_level > 1) {
        DisplayAdaptedChar(Blob, denorm, IClass);
      }
#endif
    }
    FreeFeatureSet(FloatFeatures);
  }
}                                /* AdaptToChar */

void Classify::DisplayAdaptedChar(TBLOB* blob, const DENORM& denorm,
                                  INT_CLASS_STRUCT* int_class) {
#ifndef GRAPHICS_DISABLED
  int bloblength = 0;
  INT_FEATURE_ARRAY features;
  uinT8* norm_array = new uinT8[unicharset.size()];
  int num_features = GetBaselineFeatures(blob, denorm, PreTrainedTemplates,
                                         features,
                                         norm_array, &bloblength);
  delete [] norm_array;
  INT_RESULT_STRUCT IntResult;

  im_.Match(int_class, AllProtosOn, AllConfigsOn,
            num_features, features,
            &IntResult, classify_adapt_feature_threshold,
            NO_DEBUG, matcher_debug_separate_windows);
  cprintf ("Best match to temp config %d = %4.1f%%.\n",
    IntResult.Config, (1.0 - IntResult.Rating) * 100.0);
  if (classify_learning_debug_level >= 2) {
    uinT32 ConfigMask;
    ConfigMask = 1 << IntResult.Config;
    ShowMatchDisplay();
    im_.Match(int_class, AllProtosOn, (BIT_VECTOR)&ConfigMask,
              num_features, features,
              &IntResult, classify_adapt_feature_threshold,
              6 | 0x19, matcher_debug_separate_windows);
    UpdateMatchDisplay();
  }
#endif
}


/*---------------------------------------------------------------------------*/
/**
 * @param Blob blob to add to templates for ClassId
 * @param denorm normalization/denormalization parameters
 * @param ClassId class to add blob to
 * @param FontinfoId font information from pre-trained teamples
 * @param Threshold minimum match rating to existing template
 *
 * Globals:
 * - PreTrainedTemplates current set of built-in templates
 *
 * @note Exceptions: none
 * @note History: Thu Mar 14 09:36:03 1991, DSJ, Created.
 */
void Classify::AdaptToPunc(TBLOB *Blob,
                           const DENORM& denorm,
                           CLASS_ID ClassId,
                           int FontinfoId,
                           FLOAT32 Threshold) {
  ADAPT_RESULTS *Results = new ADAPT_RESULTS();
  int i;

  Results->Initialize();
  CharNormClassifier(Blob, denorm, PreTrainedTemplates, Results);
  RemoveBadMatches(Results);

  if (Results->NumMatches != 1) {
    if (classify_learning_debug_level >= 1) {
      cprintf ("Rejecting punc = %s (Alternatives = ",
               unicharset.id_to_unichar(ClassId));

      for (i = 0; i < Results->NumMatches; i++)
        tprintf("%s", unicharset.id_to_unichar(Results->match[i].unichar_id));
      tprintf(")\n");
    }
  } else {
    #ifndef SECURE_NAMES
    if (classify_learning_debug_level >= 1)
      cprintf ("Adapting to punc = %s, thr= %g\n",
               unicharset.id_to_unichar(ClassId), Threshold);
    #endif
    AdaptToChar(Blob, denorm, ClassId, FontinfoId, Threshold);
  }
  delete Results;
}                                /* AdaptToPunc */


/*---------------------------------------------------------------------------*/
/**
 * This routine adds the result of a classification into
 * Results.  If the new rating is much worse than the current
 * best rating, it is not entered into results because it
 * would end up being stripped later anyway.  If the new rating
 * is better than the old rating for the class, it replaces the
 * old rating.  If this is the first rating for the class, the
 * class is added to the list of matched classes in Results.
 * If the new rating is better than the best so far, it
 * becomes the best so far.
 *
 * Globals:
 * - #matcher_bad_match_pad defines limits of an acceptable match
 *
 * @param[out] results results to add new result to
 * @param class_id class of new result
 * @param shape_id shape index
 * @param rating rating of new result
 * @param adapted adapted match or not
 * @param config config id of new result
 * @param fontinfo_id font information of the new result
 * @param fontinfo_id2 font information of the 2nd choice result
 *
 * @note Exceptions: none
 * @note History: Tue Mar 12 18:19:29 1991, DSJ, Created.
 */
void Classify::AddNewResult(ADAPT_RESULTS *results,
                            CLASS_ID class_id,
                            int shape_id,
                            FLOAT32 rating,
                            bool adapted,
                            int config,
                            int fontinfo_id,
                            int fontinfo_id2) {
  ScoredClass *old_match = FindScoredUnichar(results, class_id);
  ScoredClass match =
      { class_id,
        shape_id,
        rating,
        adapted,
        static_cast<inT16>(config),
        static_cast<inT16>(fontinfo_id),
        static_cast<inT16>(fontinfo_id2) };

  if (rating > results->best_match.rating + matcher_bad_match_pad ||
      (old_match && rating >= old_match->rating))
    return;

  if (!unicharset.get_fragment(class_id))
    results->HasNonfragment = true;

  if (old_match)
    old_match->rating = rating;
  else
    results->match[results->NumMatches++] = match;

  if (rating < results->best_match.rating &&
      // Ensure that fragments do not affect best rating, class and config.
      // This is needed so that at least one non-fragmented character is
      // always present in the results.
      // TODO(daria): verify that this helps accuracy and does not
      // hurt performance.
      !unicharset.get_fragment(class_id)) {
    results->best_match = match;
  }
}                                /* AddNewResult */


/*---------------------------------------------------------------------------*/
/**
 * This routine is identical to CharNormClassifier()
 * except that it does no class pruning.  It simply matches
 * the unknown blob against the classes listed in
 * Ambiguities.
 *
 * Globals:
 * - #AllProtosOn mask that enables all protos
 * - #AllConfigsOn mask that enables all configs
 *
 * @param Blob blob to be classified
 * @param denorm normalization/denormalization parameters
 * @param Templates built-in templates to classify against
 * @param Classes adapted class templates
 * @param Ambiguities array of class id's to match against
 * @param[out] Results place to put match results
 *
 * @note Exceptions: none
 * @note History: Tue Mar 12 19:40:36 1991, DSJ, Created.
 */
void Classify::AmbigClassifier(TBLOB *Blob,
                               const DENORM& denorm,
                               INT_TEMPLATES Templates,
                               ADAPT_CLASS *Classes,
                               UNICHAR_ID *Ambiguities,
                               ADAPT_RESULTS *Results) {
  int NumFeatures;
  INT_FEATURE_ARRAY IntFeatures;
  uinT8* CharNormArray = new uinT8[unicharset.size()];
  INT_RESULT_STRUCT IntResult;
  CLASS_ID ClassId;

  AmbigClassifierCalls++;

  NumFeatures = GetCharNormFeatures(Blob, denorm, Templates, IntFeatures,
                                    NULL, CharNormArray,
                                    &(Results->BlobLength), NULL);
  if (NumFeatures <= 0) {
    delete [] CharNormArray;
    return;
  }

  bool debug = matcher_debug_level >= 2 || classify_debug_level > 1;
  if (debug)
    tprintf("AM Matches =  ");

  int top = Blob->bounding_box().top();
  int bottom = Blob->bounding_box().bottom();
  while (*Ambiguities >= 0) {
    ClassId = *Ambiguities;

    im_.SetCharNormMatch(classify_integer_matcher_multiplier);
    im_.Match(ClassForClassId(Templates, ClassId),
              AllProtosOn, AllConfigsOn,
              NumFeatures, IntFeatures,
              &IntResult,
              classify_adapt_feature_threshold, NO_DEBUG,
              matcher_debug_separate_windows);

    ExpandShapesAndApplyCorrections(NULL, debug, ClassId, bottom, top, 0,
                                    Results->BlobLength, CharNormArray,
                                    IntResult, Results);
    Ambiguities++;

    NumAmbigClassesTried++;
  }
  delete [] CharNormArray;
}                                /* AmbigClassifier */

/*---------------------------------------------------------------------------*/
/// Factored-out calls to IntegerMatcher based on class pruner results.
/// Returns integer matcher results inside CLASS_PRUNER_RESULTS structure.
void Classify::MasterMatcher(INT_TEMPLATES templates,
                             inT16 num_features,
                             const INT_FEATURE_STRUCT* features,
                             const uinT8* norm_factors,
                             ADAPT_CLASS* classes,
                             int debug,
                             int num_classes,
                             const TBOX& blob_box,
                             CLASS_PRUNER_RESULTS results,
                             ADAPT_RESULTS* final_results) {
  int top = blob_box.top();
  int bottom = blob_box.bottom();
  for (int c = 0; c < num_classes; c++) {
    CLASS_ID class_id = results[c].Class;
    INT_RESULT_STRUCT& int_result = results[c].IMResult;
    BIT_VECTOR protos = classes != NULL ? classes[class_id]->PermProtos
                                        : AllProtosOn;
    BIT_VECTOR configs = classes != NULL ? classes[class_id]->PermConfigs
                                         : AllConfigsOn;

    im_.Match(ClassForClassId(templates, class_id),
              protos, configs,
              num_features, features,
              &int_result, classify_adapt_feature_threshold, debug,
              matcher_debug_separate_windows);
    bool debug = matcher_debug_level >= 2 || classify_debug_level > 1;
    ExpandShapesAndApplyCorrections(classes, debug, class_id, bottom, top,
                                    results[c].Rating,
                                    final_results->BlobLength, norm_factors,
                                    int_result, final_results);
  }
}

// Converts configs to fonts, and if the result is not adapted, and a
// shape_table_ is present, the shape is expanded to include all
// unichar_ids represented, before applying a set of corrections to the
// distance rating in int_result, (see ComputeCorrectedRating.)
// The results are added to the final_results output.
void Classify::ExpandShapesAndApplyCorrections(
    ADAPT_CLASS* classes, bool debug, int class_id, int bottom, int top,
    float cp_rating, int blob_length, const uinT8* cn_factors,
    INT_RESULT_STRUCT& int_result, ADAPT_RESULTS* final_results) {
  // Compute the fontinfo_ids.
  int fontinfo_id = kBlankFontinfoId;
  int fontinfo_id2 = kBlankFontinfoId;
  if (classes != NULL) {
    // Adapted result.
    fontinfo_id = GetFontinfoId(classes[class_id], int_result.Config);
    if (int_result.Config2 >= 0)
      fontinfo_id2 = GetFontinfoId(classes[class_id], int_result.Config2);
  } else {
    // Pre-trained result.
    fontinfo_id = ClassAndConfigIDToFontOrShapeID(class_id, int_result.Config);
    if (int_result.Config2 >= 0) {
      fontinfo_id2 = ClassAndConfigIDToFontOrShapeID(class_id,
                                                     int_result.Config2);
    }
    if (shape_table_ != NULL) {
      // Actually fontinfo_id is an index into the shape_table_ and it
      // contains a list of unchar_id/font_id pairs.
      int shape_id = fontinfo_id;
      const Shape& shape = shape_table_->GetShape(fontinfo_id);
      double min_rating = 0.0;
      for (int c = 0; c < shape.size(); ++c) {
        int unichar_id = shape[c].unichar_id;
        fontinfo_id = shape[c].font_ids[0];
        if (shape[c].font_ids.size() > 1)
          fontinfo_id2 = shape[c].font_ids[1];
        else if (fontinfo_id2 != kBlankFontinfoId)
          fontinfo_id2 = shape_table_->GetShape(fontinfo_id2)[0].font_ids[0];
        double rating = ComputeCorrectedRating(debug, unichar_id, cp_rating,
                                               int_result.Rating,
                                               int_result.FeatureMisses,
                                               bottom, top, blob_length,
                                               cn_factors);
        if (c == 0 || rating < min_rating)
          min_rating = rating;
        if (unicharset.get_enabled(unichar_id)) {
          AddNewResult(final_results, unichar_id, shape_id, rating,
                       classes != NULL, int_result.Config,
                       fontinfo_id, fontinfo_id2);
        }
      }
      int_result.Rating = min_rating;
      return;
    }
  }
  double rating = ComputeCorrectedRating(debug, class_id, cp_rating,
                                         int_result.Rating,
                                         int_result.FeatureMisses,
                                         bottom, top, blob_length,
                                         cn_factors);
  if (unicharset.get_enabled(class_id)) {
    AddNewResult(final_results, class_id, -1, rating,
                 classes != NULL, int_result.Config,
                 fontinfo_id, fontinfo_id2);
  }
  int_result.Rating = rating;
}

// Applies a set of corrections to the distance im_rating,
// including the cn_correction, miss penalty and additional penalty
// for non-alnums being vertical misfits. Returns the corrected distance.
double Classify::ComputeCorrectedRating(bool debug, int unichar_id,
                                        double cp_rating, double im_rating,
                                        int feature_misses,
                                        int bottom, int top,
                                        int blob_length,
                                        const uinT8* cn_factors) {
  // Compute class feature corrections.
  double cn_corrected = im_.ApplyCNCorrection(im_rating, blob_length,
                                              cn_factors[unichar_id]);
  double miss_penalty = tessedit_class_miss_scale * feature_misses;
  double vertical_penalty = 0.0;
  // Penalize non-alnums for being vertical misfits.
  if (!unicharset.get_isalpha(unichar_id) &&
      !unicharset.get_isdigit(unichar_id) &&
      cn_factors[unichar_id] != 0 && classify_misfit_junk_penalty > 0.0) {
    int min_bottom, max_bottom, min_top, max_top;
    unicharset.get_top_bottom(unichar_id, &min_bottom, &max_bottom,
                              &min_top, &max_top);
    if (debug) {
      tprintf("top=%d, vs [%d, %d], bottom=%d, vs [%d, %d]\n",
              top, min_top, max_top, bottom, min_bottom, max_bottom);
    }
    if (top < min_top || top > max_top ||
        bottom < min_bottom || bottom > max_bottom) {
      vertical_penalty = classify_misfit_junk_penalty;
    }
  }
  double result =cn_corrected + miss_penalty + vertical_penalty;
  if (result > WORST_POSSIBLE_RATING)
    result = WORST_POSSIBLE_RATING;
  if (debug) {
    tprintf("%s: %2.1f(CP%2.1f, IM%2.1f + CN%.2f(%d) + MP%2.1f + VP%2.1f)\n",
            unicharset.id_to_unichar(unichar_id),
            result * 100.0,
            cp_rating * 100.0,
            im_rating * 100.0,
            (cn_corrected - im_rating) * 100.0,
            cn_factors[unichar_id],
            miss_penalty * 100.0,
            vertical_penalty * 100.0);
  }
  return result;
}

/*---------------------------------------------------------------------------*/
/**
 * This routine extracts baseline normalized features
 * from the unknown character and matches them against the
 * specified set of templates.  The classes which match
 * are added to Results.
 *
 * Globals:
 * - BaselineCutoffs expected num features for each class
 *
 * @param Blob blob to be classified
 * @param denorm normalization/denormalization parameters
 * @param Templates current set of adapted templates
 * @param Results place to put match results
 *
 * @return Array of possible ambiguous chars that should be checked.
 * @note Exceptions: none
 * @note History: Tue Mar 12 19:38:03 1991, DSJ, Created.
 */
UNICHAR_ID *Classify::BaselineClassifier(TBLOB *Blob,
                                         const DENORM& denorm,
                                         ADAPT_TEMPLATES Templates,
                                         ADAPT_RESULTS *Results) {
  int NumFeatures;
  int NumClasses;
  INT_FEATURE_ARRAY IntFeatures;
  uinT8* CharNormArray = new uinT8[unicharset.size()];
  CLASS_ID ClassId;

  BaselineClassifierCalls++;

  NumFeatures = GetBaselineFeatures(
      Blob, denorm, Templates->Templates, IntFeatures, CharNormArray,
      &(Results->BlobLength));
  if (NumFeatures <= 0) {
    delete [] CharNormArray;
    return NULL;
  }

  NumClasses = PruneClasses(Templates->Templates, NumFeatures, IntFeatures,
                            CharNormArray, BaselineCutoffs, Results->CPResults);

  NumBaselineClassesTried += NumClasses;

  if (matcher_debug_level >= 2 || classify_debug_level > 1)
    cprintf ("BL Matches =  ");

  im_.SetBaseLineMatch();
  MasterMatcher(Templates->Templates, NumFeatures, IntFeatures, CharNormArray,
                Templates->Class, matcher_debug_flags, NumClasses,
                Blob->bounding_box(), Results->CPResults, Results);

  delete [] CharNormArray;
  ClassId = Results->best_match.unichar_id;
  if (ClassId == NO_CLASS)
    return (NULL);
  /* this is a bug - maybe should return "" */

  return Templates->Class[ClassId]->
      Config[Results->best_match.config].Perm->Ambigs;
}                                /* BaselineClassifier */


/*---------------------------------------------------------------------------*/
/**
 * This routine extracts character normalized features
 * from the unknown character and matches them against the
 * specified set of templates.  The classes which match
 * are added to Results.
 *
 * @param Blob blob to be classified
 * @param denorm normalization/denormalization parameters
 * @param Templates templates to classify unknown against
 * @param Results place to put match results
 *
 * Globals:
 * - CharNormCutoffs expected num features for each class
 * - AllProtosOn mask that enables all protos
 * - AllConfigsOn mask that enables all configs
 *
 * @note Exceptions: none
 * @note History: Tue Mar 12 16:02:52 1991, DSJ, Created.
 */
int Classify::CharNormClassifier(TBLOB *Blob,
                                 const DENORM& denorm,
                                 INT_TEMPLATES Templates,
                                 ADAPT_RESULTS *Results) {
  int NumFeatures;
  int NumClasses;
  INT_FEATURE_ARRAY IntFeatures;

  CharNormClassifierCalls++;

  uinT8* CharNormArray = new uinT8[unicharset.size()];
  int num_pruner_classes = MAX(unicharset.size(),
                               PreTrainedTemplates->NumClasses);
  uinT8* PrunerNormArray = new uinT8[num_pruner_classes];
  NumFeatures = GetCharNormFeatures(Blob, denorm, Templates, IntFeatures,
                                    PrunerNormArray, CharNormArray,
                                    &(Results->BlobLength), NULL);
  if (NumFeatures <= 0) {
    delete [] CharNormArray;
    delete [] PrunerNormArray;
    return 0;
  }

  NumClasses = PruneClasses(Templates, NumFeatures, IntFeatures,
                            PrunerNormArray,
                            shape_table_ != NULL ? &shapetable_cutoffs_[0]
                                                 : CharNormCutoffs,
                            Results->CPResults);

  if (tessedit_single_match && NumClasses > 1)
    NumClasses = 1;
  NumCharNormClassesTried += NumClasses;

  im_.SetCharNormMatch(classify_integer_matcher_multiplier);
  MasterMatcher(Templates, NumFeatures, IntFeatures, CharNormArray,
                NULL, matcher_debug_flags, NumClasses,
                Blob->bounding_box(), Results->CPResults, Results);
  delete [] CharNormArray;
  delete [] PrunerNormArray;
  return NumFeatures;
}                                /* CharNormClassifier */

// As CharNormClassifier, but operates on a TrainingSample and outputs to
// a GenericVector of ShapeRating without conversion to classes.
int Classify::CharNormTrainingSample(bool pruner_only,
                                     const TrainingSample& sample,
                                     GenericVector<ShapeRating>* results) {
  results->clear();
  ADAPT_RESULTS* adapt_results = new ADAPT_RESULTS();
  adapt_results->Initialize();
  // Compute the bounding box of the features.
  int num_features = sample.num_features();
  TBOX blob_box;
  for (int f = 0; f < num_features; ++f) {
    const INT_FEATURE_STRUCT feature = sample.features()[f];
    TBOX fbox(feature.X, feature.Y, feature.X, feature.Y);
    blob_box += fbox;
  }
  // Compute the char_norm_array from the saved cn_feature.
  FEATURE norm_feature = NewFeature(&CharNormDesc);
  norm_feature->Params[CharNormY] = sample.cn_feature(CharNormY);
  norm_feature->Params[CharNormLength] = sample.cn_feature(CharNormLength);
  norm_feature->Params[CharNormRx] = sample.cn_feature(CharNormRx);
  norm_feature->Params[CharNormRy] = sample.cn_feature(CharNormRy);
  uinT8* char_norm_array = new uinT8[unicharset.size()];
  int num_pruner_classes = MAX(unicharset.size(),
                               PreTrainedTemplates->NumClasses);
  uinT8* pruner_norm_array = new uinT8[num_pruner_classes];
  adapt_results->BlobLength =
      static_cast<int>(ActualOutlineLength(norm_feature) * 20 + 0.5);
  ComputeCharNormArrays(norm_feature, PreTrainedTemplates, char_norm_array,
                        pruner_norm_array);

  int num_classes = PruneClasses(PreTrainedTemplates, num_features,
                                 sample.features(),
                                 pruner_norm_array,
                                 shape_table_ != NULL ? &shapetable_cutoffs_[0]
                                                      : CharNormCutoffs,
                                 adapt_results->CPResults);
  delete [] pruner_norm_array;
  if (pruner_only) {
    // Convert pruner results to output format.
    for (int i = 0; i < num_classes; ++i) {
      int class_id = adapt_results->CPResults[i].Class;
      int shape_id = class_id;
      if (shape_table_ != NULL) {
        // All shapes in a class have the same combination of unichars, so
        // it doesn't really matter which config we give it, as we aren't
        // trying to get the font here.
        shape_id = ClassAndConfigIDToFontOrShapeID(class_id, 0);
      }
      results->push_back(
          ShapeRating(shape_id, 1.0f - adapt_results->CPResults[i].Rating));
    }
  } else {
    im_.SetCharNormMatch(classify_integer_matcher_multiplier);
    MasterMatcher(PreTrainedTemplates, num_features, sample.features(),
                  char_norm_array,
                  NULL, matcher_debug_flags, num_classes,
                  blob_box, adapt_results->CPResults, adapt_results);
    // Convert master matcher results to output format.
    for (int i = 0; i < adapt_results->NumMatches; i++) {
      ScoredClass next = adapt_results->match[i];
      results->push_back(ShapeRating(next.shape_id, 1.0f - next.rating));
    }
    results->sort(&ShapeRating::SortDescendingRating);
  }
  delete [] char_norm_array;
  delete adapt_results;
  return num_features;
}                                /* CharNormTrainingSample */


/*---------------------------------------------------------------------------*/
/**
 * This routine computes a rating which reflects the
 * likelihood that the blob being classified is a noise
 * blob.  NOTE: assumes that the blob length has already been
 * computed and placed into Results.
 *
 * @param Results results to add noise classification to
 *
 * Globals:
 * - matcher_avg_noise_size avg. length of a noise blob
 *
 * @note Exceptions: none
 * @note History: Tue Mar 12 18:36:52 1991, DSJ, Created.
 */
void Classify::ClassifyAsNoise(ADAPT_RESULTS *Results) {
  register FLOAT32 Rating;

  Rating = Results->BlobLength / matcher_avg_noise_size;
  Rating *= Rating;
  Rating /= 1.0 + Rating;

  AddNewResult(Results, NO_CLASS, -1, Rating, false, -1,
               kBlankFontinfoId, kBlankFontinfoId);
}                                /* ClassifyAsNoise */
}  // namespace tesseract


/*---------------------------------------------------------------------------*/
// Return a pointer to the scored unichar in results, or NULL if not present.
ScoredClass *FindScoredUnichar(ADAPT_RESULTS *results, UNICHAR_ID id) {
  for (int i = 0; i < results->NumMatches; i++) {
    if (results->match[i].unichar_id == id)
      return &results->match[i];
  }
  return NULL;
}

// Retrieve the current rating for a unichar id if we have rated it, defaulting
// to WORST_POSSIBLE_RATING.
ScoredClass ScoredUnichar(ADAPT_RESULTS *results, UNICHAR_ID id) {
  ScoredClass poor_result =
      {id, -1, WORST_POSSIBLE_RATING, false, -1,
          kBlankFontinfoId, kBlankFontinfoId};
  ScoredClass *entry = FindScoredUnichar(results, id);
  return (entry == NULL) ? poor_result : *entry;
}

// Compare character classes by rating as for qsort(3).
// For repeatability, use character class id as a tie-breaker.
int CompareByRating(const void *arg1,    // ScoredClass *class1
                    const void *arg2) {  // ScoredClass *class2
  const ScoredClass *class1 = (const ScoredClass *)arg1;
  const ScoredClass *class2 = (const ScoredClass *)arg2;

  if (class1->rating < class2->rating)
    return -1;
  else if (class1->rating > class2->rating)
    return 1;

  if (class1->unichar_id < class2->unichar_id)
    return -1;
  else if (class1->unichar_id > class2->unichar_id)
    return 1;
  return 0;
}

/*---------------------------------------------------------------------------*/
namespace tesseract {
/// The function converts the given match ratings to the list of blob
/// choices with ratings and certainties (used by the context checkers).
/// If character fragments are present in the results, this function also makes
/// sure that there is at least one non-fragmented classification included.
/// For each classification result check the unicharset for "definite"
/// ambiguities and modify the resulting Choices accordingly.
void Classify::ConvertMatchesToChoices(const DENORM& denorm, const TBOX& box,
                                       ADAPT_RESULTS *Results,
                                       BLOB_CHOICE_LIST *Choices) {
  assert(Choices != NULL);
  FLOAT32 Rating;
  FLOAT32 Certainty;
  BLOB_CHOICE_IT temp_it;
  bool contains_nonfrag = false;
  temp_it.set_to_list(Choices);
  int choices_length = 0;
  // With no shape_table_ maintain the previous MAX_MATCHES as the maximum
  // number of returned results, but with a shape_table_ we want to have room
  // for at least the biggest shape (which might contain hundreds of Indic
  // grapheme fragments) and more, so use double the size of the biggest shape
  // if that is more than the default.
  int max_matches = MAX_MATCHES;
  if (shape_table_ != NULL) {
    max_matches = shape_table_->MaxNumUnichars() * 2;
    if (max_matches < MAX_MATCHES)
      max_matches = MAX_MATCHES;
  }

  for (int i = 0; i < Results->NumMatches; i++) {
    ScoredClass next = Results->match[i];
    int fontinfo_id = next.fontinfo_id;
    int fontinfo_id2 = next.fontinfo_id2;
    bool adapted = next.adapted;
    bool current_is_frag = (unicharset.get_fragment(next.unichar_id) != NULL);
    if (temp_it.length()+1 == max_matches &&
        !contains_nonfrag && current_is_frag) {
      continue;  // look for a non-fragmented character to fill the
                 // last spot in Choices if only fragments are present
    }
    // BlobLength can never be legally 0, this means recognition failed.
    // But we must return a classification result because some invoking
    // functions (chopper/permuter) do not anticipate a null blob choice.
    // So we need to assign a poor, but not infinitely bad score.
    if (Results->BlobLength == 0) {
      Certainty = -20;
      Rating = 100;    // should be -certainty * real_blob_length
    } else {
      Rating = Certainty = next.rating;
      Rating *= rating_scale * Results->BlobLength;
      Certainty *= -(getDict().certainty_scale);
    }
    inT16 min_xheight, max_xheight;
    denorm.XHeightRange(next.unichar_id, unicharset, box,
                        &min_xheight, &max_xheight);
    temp_it.add_to_end(new BLOB_CHOICE(next.unichar_id, Rating, Certainty,
                                        fontinfo_id, fontinfo_id2,
                                        unicharset.get_script(next.unichar_id),
                                        min_xheight, max_xheight, adapted));
    contains_nonfrag |= !current_is_frag;  // update contains_nonfrag
    choices_length++;
    if (choices_length >= max_matches) break;
  }
  Results->NumMatches = choices_length;
}  // ConvertMatchesToChoices


/*---------------------------------------------------------------------------*/
#ifndef GRAPHICS_DISABLED
/**
 *
 * @param Blob blob whose classification is being debugged
 * @param denorm normalization/denormalization parameters
 * @param Results results of match being debugged
 *
 * Globals: none
 *
 * @note Exceptions: none
 * @note History: Wed Mar 13 16:44:41 1991, DSJ, Created.
 */
void Classify::DebugAdaptiveClassifier(TBLOB *Blob,
                                       const DENORM& denorm,
                                       ADAPT_RESULTS *Results) {
  for (int i = 0; i < Results->NumMatches; i++) {
    if (Results->match[i].rating < Results->best_match.rating)
      Results->best_match = Results->match[i];
  }
  const char *Prompt =
    "Left-click in IntegerMatch Window to continue or right click to debug...";
  CLASS_ID unichar_id = Results->best_match.unichar_id;
  int shape_id = Results->best_match.shape_id;
  bool adaptive_on = true;
  bool pretrained_on = true;

  const char* debug_mode;
  do {
    if (!pretrained_on)
      debug_mode = "Adaptive Templates Only";
    else if (!adaptive_on)
      debug_mode = "PreTrained Templates Only";
    else
      debug_mode = "All Templates";
    ShowMatchDisplay();
    tprintf("Debugging class %d = %s in mode %s ...",
            unichar_id, unicharset.id_to_unichar(unichar_id), debug_mode);
    if (shape_id >= 0 && shape_table_ != NULL) {
      tprintf(" from shape %s\n", shape_table_->DebugStr(shape_id).string());
    }
    ShowBestMatchFor(Blob, denorm, unichar_id, shape_id, adaptive_on,
                     pretrained_on, Results);
    UpdateMatchDisplay();
  } while ((unichar_id = GetClassToDebug(Prompt, &adaptive_on,
                                         &pretrained_on, &shape_id)) != 0);
}                                /* DebugAdaptiveClassifier */
#endif

/*---------------------------------------------------------------------------*/
/**
 * This routine performs an adaptive classification.
 * If we have not yet adapted to enough classes, a simple
 * classification to the pre-trained templates is performed.
 * Otherwise, we match the blob against the adapted templates.
 * If the adapted templates do not match well, we try a
 * match against the pre-trained templates.  If an adapted
 * template match is found, we do a match to any pre-trained
 * templates which could be ambiguous.  The results from all
 * of these classifications are merged together into Results.
 *
 * @param Blob blob to be classified
 * @param denorm normalization/denormalization parameters
 * @param Results place to put match results
 *
 * Globals:
 * - PreTrainedTemplates built-in training templates
 * - AdaptedTemplates templates adapted for this page
 * - matcher_great_threshold rating limit for a great match
 *
 * @note Exceptions: none
 * @note History: Tue Mar 12 08:50:11 1991, DSJ, Created.
 */
void Classify::DoAdaptiveMatch(TBLOB *Blob,
                               const DENORM& denorm,
                               ADAPT_RESULTS *Results) {
  UNICHAR_ID *Ambiguities;

  AdaptiveMatcherCalls++;
  InitIntFX();

  if (AdaptedTemplates->NumPermClasses < matcher_permanent_classes_min ||
      tess_cn_matching) {
    CharNormClassifier(Blob, denorm, PreTrainedTemplates, Results);
  } else {
    Ambiguities = BaselineClassifier(Blob, denorm, AdaptedTemplates, Results);
    if ((Results->NumMatches > 0 &&
         MarginalMatch (Results->best_match.rating) &&
         !tess_bn_matching) ||
        Results->NumMatches == 0) {
      CharNormClassifier(Blob, denorm, PreTrainedTemplates, Results);
    } else if (Ambiguities && *Ambiguities >= 0 && !tess_bn_matching) {
      AmbigClassifier(Blob, denorm,
                      PreTrainedTemplates,
                      AdaptedTemplates->Class,
                      Ambiguities,
                      Results);
    }
  }

  // Force the blob to be classified as noise
  // if the results contain only fragments.
  // TODO(daria): verify that this is better than
  // just adding a NULL classification.
  if (!Results->HasNonfragment || Results->NumMatches == 0)
    ClassifyAsNoise(Results);
}   /* DoAdaptiveMatch */

/*---------------------------------------------------------------------------*/
/**
 * This routine tries to estimate how tight the adaptation
 * threshold should be set for each character in the current
 * word.  In general, the routine tries to set tighter
 * thresholds for a character when the current set of templates
 * would have made an error on that character.  It tries
 * to set a threshold tight enough to eliminate the error.
 * Two different sets of rules can be used to determine the
 * desired thresholds.
 *
 * @param Word current word
 * @param denorm normalization/denormalization parameters
 * @param BestChoice best choice for current word with context
 * @param BestRawChoice best choice for current word without context
 * @param[out] Thresholds array of thresholds to be filled in
 *
 * Globals:
 * - matcher_good_threshold
 * - matcher_perfect_threshold
 * - matcher_rating_margin
 *
 * @return none (results are returned in Thresholds)
 * @note Exceptions: none
 * @note History: Fri May 31 09:22:08 1991, DSJ, Created.
 */
void Classify::GetAdaptThresholds(TWERD * Word,
                                  const DENORM& denorm,
                                  const WERD_CHOICE& BestChoice,
                                  const WERD_CHOICE& BestRawChoice,
                                  FLOAT32 Thresholds[]) {
  getDict().FindClassifierErrors(matcher_perfect_threshold,
                                 matcher_good_threshold,
                                 matcher_rating_margin,
                                 Thresholds);
}                              /* GetAdaptThresholds */

/*---------------------------------------------------------------------------*/
/**
 * This routine matches blob to the built-in templates
 * to find out if there are any classes other than the correct
 * class which are potential ambiguities.
 *
 * @param Blob blob to get classification ambiguities for
 * @param denorm normalization/denormalization parameters
 * @param CorrectClass correct class for Blob
 *
 * Globals:
 * - CurrentRatings used by qsort compare routine
 * - PreTrainedTemplates built-in templates
 *
 * @return String containing all possible ambiguous classes.
 * @note Exceptions: none
 * @note History: Fri Mar 15 08:08:22 1991, DSJ, Created.
 */
UNICHAR_ID *Classify::GetAmbiguities(TBLOB *Blob,
                                     const DENORM& denorm,
                                     CLASS_ID CorrectClass) {
  ADAPT_RESULTS *Results = new ADAPT_RESULTS();
  UNICHAR_ID *Ambiguities;
  int i;

  Results->Initialize();

  CharNormClassifier(Blob, denorm, PreTrainedTemplates, Results);
  RemoveBadMatches(Results);
  qsort((void *)Results->match, Results->NumMatches,
        sizeof(ScoredClass), CompareByRating);

  /* copy the class id's into an string of ambiguities - don't copy if
     the correct class is the only class id matched */
  Ambiguities = (UNICHAR_ID *) Emalloc (sizeof (UNICHAR_ID) *
                                        (Results->NumMatches + 1));
  if (Results->NumMatches > 1 ||
      (Results->NumMatches == 1 &&
          Results->match[0].unichar_id != CorrectClass)) {
    for (i = 0; i < Results->NumMatches; i++)
      Ambiguities[i] = Results->match[i].unichar_id;
    Ambiguities[i] = -1;
  } else {
    Ambiguities[0] = -1;
  }

  delete Results;
  return Ambiguities;
}                              /* GetAmbiguities */

/*---------------------------------------------------------------------------*/
/**
 * This routine calls the integer (Hardware) feature
 * extractor if it has not been called before for this blob.
 * The results from the feature extractor are placed into
 * globals so that they can be used in other routines without
 * re-extracting the features.
 * It then copies the baseline features into the IntFeatures
 * array provided by the caller.
 *
 * @param Blob blob to extract features from
 * @param denorm normalization/denormalization parameters
 * @param Templates used to compute char norm adjustments
 * @param IntFeatures array to fill with integer features
 * @param CharNormArray array to fill with dummy char norm adjustments
 * @param BlobLength length of blob in baseline-normalized units
 *
 * Globals:
 * - FeaturesHaveBeenExtracted TRUE if fx has been done
 * - BaselineFeatures holds extracted baseline feat
 * - CharNormFeatures holds extracted char norm feat
 * - FXInfo holds misc. FX info
 *
 * @return Number of features extracted or 0 if an error occured.
 * @note Exceptions: none
 * @note History: Tue May 28 10:40:52 1991, DSJ, Created.
 */
int Classify::GetBaselineFeatures(TBLOB *Blob,
                                  const DENORM& denorm,
                                  INT_TEMPLATES Templates,
                                  INT_FEATURE_ARRAY IntFeatures,
                                  uinT8* CharNormArray,
                                  inT32 *BlobLength) {
  register INT_FEATURE Src, Dest, End;

  if (!FeaturesHaveBeenExtracted) {
    FeaturesOK = ExtractIntFeat(Blob, denorm, BaselineFeatures,
                                CharNormFeatures, &FXInfo, NULL);
    FeaturesHaveBeenExtracted = TRUE;
  }

  if (!FeaturesOK) {
    *BlobLength = FXInfo.NumBL;
    return 0;
  }

  for (Src = BaselineFeatures, End = Src + FXInfo.NumBL, Dest = IntFeatures;
       Src < End;
       *Dest++ = *Src++);

  ClearCharNormArray(CharNormArray);
  *BlobLength = FXInfo.NumBL;
  return FXInfo.NumBL;
}                              /* GetBaselineFeatures */

void Classify::ResetFeaturesHaveBeenExtracted() {
  FeaturesHaveBeenExtracted = FALSE;
}

// Returns true if the given blob looks too dissimilar to any character
// present in the classifier templates.
bool Classify::LooksLikeGarbage(const DENORM& denorm, TBLOB *blob) {
  BLOB_CHOICE_LIST *ratings = new BLOB_CHOICE_LIST();
  AdaptiveClassifier(blob, denorm, ratings, NULL);
  BLOB_CHOICE_IT ratings_it(ratings);
  const UNICHARSET &unicharset = getDict().getUnicharset();
  if (classify_debug_character_fragments) {
    print_ratings_list("======================\nLooksLikeGarbage() got ",
                       ratings, unicharset);
  }
  for (ratings_it.mark_cycle_pt(); !ratings_it.cycled_list();
       ratings_it.forward()) {
    if (unicharset.get_fragment(ratings_it.data()->unichar_id()) != NULL) {
      continue;
    }
    delete ratings;
    return (ratings_it.data()->certainty() <
            classify_character_fragments_garbage_certainty_threshold);
  }
  delete ratings;
  return true;  // no whole characters in ratings
}

/*---------------------------------------------------------------------------*/
/**
 * This routine calls the integer (Hardware) feature
 * extractor if it has not been called before for this blob.
 *
 * The results from the feature extractor are placed into
 * globals so that they can be used in other routines without
 * re-extracting the features.
 *
 * It then copies the char norm features into the IntFeatures
 * array provided by the caller.
 *
 * @param Blob blob to extract features from
 * @param denorm normalization/denormalization parameters
 * @param Templates used to compute char norm adjustments
 * @param IntFeatures array to fill with integer features
 * @param PrunerNormArray Array of factors from blob normalization
 *        process
 * @param CharNormArray array to fill with dummy char norm adjustments
 * @param BlobLength length of blob in baseline-normalized units
 * @param FeatureOutlineArray
 *
 * Globals:
 * - FeaturesHaveBeenExtracted TRUE if fx has been done
 * - BaselineFeatures holds extracted baseline feat
 * - CharNormFeatures holds extracted char norm feat
 * - FXInfo holds misc. FX info
 *
 * @return Number of features extracted or 0 if an error occured.
 * @note Exceptions: none
 * @note History: Tue May 28 10:40:52 1991, DSJ, Created.
 */
int Classify::GetCharNormFeatures(TBLOB *Blob,
                                  const DENORM& denorm,
                                  INT_TEMPLATES Templates,
                                  INT_FEATURE_ARRAY IntFeatures,
                                  uinT8* PrunerNormArray,
                                  uinT8* CharNormArray,
                                  inT32 *BlobLength,
                                  inT32 *FeatureOutlineArray) {
  register INT_FEATURE Src, Dest, End;
  FEATURE NormFeature;
  FLOAT32 Baseline, Scale;
  inT32 FeatureOutlineIndex[MAX_NUM_INT_FEATURES];

  if (!FeaturesHaveBeenExtracted) {
    FeaturesOK = ExtractIntFeat(Blob, denorm, BaselineFeatures,
                                CharNormFeatures, &FXInfo,
                                FeatureOutlineIndex);
    FeaturesHaveBeenExtracted = TRUE;
  }

  if (!FeaturesOK) {
    *BlobLength = FXInfo.NumBL;
    return (0);
  }

  for (Src = CharNormFeatures, End = Src + FXInfo.NumCN, Dest = IntFeatures;
       Src < End;
       *Dest++ = *Src++);
  for (int i = 0;  FeatureOutlineArray && i < FXInfo.NumCN; ++i) {
    FeatureOutlineArray[i] = FeatureOutlineIndex[i];
  }

  NormFeature = NewFeature(&CharNormDesc);
  Baseline = BASELINE_OFFSET;
  Scale = MF_SCALE_FACTOR;
  NormFeature->Params[CharNormY] = (FXInfo.Ymean - Baseline) * Scale;
  NormFeature->Params[CharNormLength] =
    FXInfo.Length * Scale / LENGTH_COMPRESSION;
  NormFeature->Params[CharNormRx] = FXInfo.Rx * Scale;
  NormFeature->Params[CharNormRy] = FXInfo.Ry * Scale;
  ComputeCharNormArrays(NormFeature, Templates, CharNormArray, PrunerNormArray);
  *BlobLength = FXInfo.NumBL;
  return (FXInfo.NumCN);
}                              /* GetCharNormFeatures */

// Computes the char_norm_array for the unicharset and, if not NULL, the
// pruner_array as appropriate according to the existence of the shape_table.
void Classify::ComputeCharNormArrays(FEATURE_STRUCT* norm_feature,
                                     INT_TEMPLATES_STRUCT* templates,
                                     uinT8* char_norm_array,
                                     uinT8* pruner_array) {
  ComputeIntCharNormArray(*norm_feature, char_norm_array);
  if (pruner_array != NULL) {
    if (shape_table_ == NULL) {
      ComputeIntCharNormArray(*norm_feature, pruner_array);
    } else {
      memset(pruner_array, MAX_UINT8,
             templates->NumClasses * sizeof(pruner_array[0]));
      // Each entry in the pruner norm array is the MIN of all the entries of
      // the corresponding unichars in the CharNormArray.
      for (int id = 0; id < templates->NumClasses; ++id) {
        int font_set_id = templates->Class[id]->font_set_id;
        const FontSet &fs = fontset_table_.get(font_set_id);
        for (int config = 0; config < fs.size; ++config) {
          const Shape& shape = shape_table_->GetShape(fs.configs[config]);
          for (int c = 0; c < shape.size(); ++c) {
            if (char_norm_array[shape[c].unichar_id] < pruner_array[id])
              pruner_array[id] = char_norm_array[shape[c].unichar_id];
          }
        }
      }
    }
  }
  FreeFeature(norm_feature);
}

/*---------------------------------------------------------------------------*/
/**
 *
 * @param Templates adapted templates to add new config to
 * @param ClassId class id to associate with new config
 * @param FontinfoId font information inferred from pre-trained templates
 * @param NumFeatures number of features in IntFeatures
 * @param Features features describing model for new config
 * @param FloatFeatures floating-pt representation of features
 *
 * @return The id of the new config created, a negative integer in
 * case of error.
 * @note Exceptions: none
 * @note History: Fri Mar 15 08:49:46 1991, DSJ, Created.
 */
int Classify::MakeNewTemporaryConfig(ADAPT_TEMPLATES Templates,
                           CLASS_ID ClassId,
                           int FontinfoId,
                           int NumFeatures,
                           INT_FEATURE_ARRAY Features,
                           FEATURE_SET FloatFeatures) {
  INT_CLASS IClass;
  ADAPT_CLASS Class;
  PROTO_ID OldProtos[MAX_NUM_PROTOS];
  FEATURE_ID BadFeatures[MAX_NUM_INT_FEATURES];
  int NumOldProtos;
  int NumBadFeatures;
  int MaxProtoId, OldMaxProtoId;
  int BlobLength = 0;
  int MaskSize;
  int ConfigId;
  TEMP_CONFIG Config;
  int i;
  int debug_level = NO_DEBUG;

  if (classify_learning_debug_level >= 3)
    debug_level =
        PRINT_MATCH_SUMMARY | PRINT_FEATURE_MATCHES | PRINT_PROTO_MATCHES;

  IClass = ClassForClassId(Templates->Templates, ClassId);
  Class = Templates->Class[ClassId];

  if (IClass->NumConfigs >= MAX_NUM_CONFIGS) {
    ++NumAdaptationsFailed;
    if (classify_learning_debug_level >= 1)
      cprintf("Cannot make new temporary config: maximum number exceeded.\n");
    return -1;
  }

  OldMaxProtoId = IClass->NumProtos - 1;

  NumOldProtos = im_.FindGoodProtos(IClass, AllProtosOn, AllConfigsOff,
                                    BlobLength, NumFeatures, Features,
                                    OldProtos, classify_adapt_proto_threshold,
                                    debug_level);

  MaskSize = WordsInVectorOfSize(MAX_NUM_PROTOS);
  zero_all_bits(TempProtoMask, MaskSize);
  for (i = 0; i < NumOldProtos; i++)
    SET_BIT(TempProtoMask, OldProtos[i]);

  NumBadFeatures = im_.FindBadFeatures(IClass, TempProtoMask, AllConfigsOn,
                                       BlobLength, NumFeatures, Features,
                                       BadFeatures,
                                       classify_adapt_feature_threshold,
                                       debug_level);

  MaxProtoId = MakeNewTempProtos(FloatFeatures, NumBadFeatures, BadFeatures,
                                 IClass, Class, TempProtoMask);
  if (MaxProtoId == NO_PROTO) {
    ++NumAdaptationsFailed;
    if (classify_learning_debug_level >= 1)
      cprintf("Cannot make new temp protos: maximum number exceeded.\n");
    return -1;
  }

  ConfigId = AddIntConfig(IClass);
  ConvertConfig(TempProtoMask, ConfigId, IClass);
  Config = NewTempConfig(MaxProtoId, FontinfoId);
  TempConfigFor(Class, ConfigId) = Config;
  copy_all_bits(TempProtoMask, Config->Protos, Config->ProtoVectorSize);

  if (classify_learning_debug_level >= 1)
    cprintf("Making new temp config %d fontinfo id %d"
            " using %d old and %d new protos.\n",
            ConfigId, Config->FontinfoId,
            NumOldProtos, MaxProtoId - OldMaxProtoId);

  return ConfigId;
}                              /* MakeNewTemporaryConfig */

/*---------------------------------------------------------------------------*/
/**
 * This routine finds sets of sequential bad features
 * that all have the same angle and converts each set into
 * a new temporary proto.  The temp proto is added to the
 * proto pruner for IClass, pushed onto the list of temp
 * protos in Class, and added to TempProtoMask.
 *
 * @param Features floating-pt features describing new character
 * @param NumBadFeat number of bad features to turn into protos
 * @param BadFeat feature id's of bad features
 * @param IClass integer class templates to add new protos to
 * @param Class adapted class templates to add new protos to
 * @param TempProtoMask proto mask to add new protos to
 *
 * Globals: none
 *
 * @return Max proto id in class after all protos have been added.
 * Exceptions: none
 * History: Fri Mar 15 11:39:38 1991, DSJ, Created.
 */
PROTO_ID Classify::MakeNewTempProtos(FEATURE_SET Features,
                                     int NumBadFeat,
                                     FEATURE_ID BadFeat[],
                                     INT_CLASS IClass,
                                     ADAPT_CLASS Class,
                                     BIT_VECTOR TempProtoMask) {
  FEATURE_ID *ProtoStart;
  FEATURE_ID *ProtoEnd;
  FEATURE_ID *LastBad;
  TEMP_PROTO TempProto;
  PROTO Proto;
  FEATURE F1, F2;
  FLOAT32 X1, X2, Y1, Y2;
  FLOAT32 A1, A2, AngleDelta;
  FLOAT32 SegmentLength;
  PROTO_ID Pid;

  for (ProtoStart = BadFeat, LastBad = ProtoStart + NumBadFeat;
       ProtoStart < LastBad; ProtoStart = ProtoEnd) {
    F1 = Features->Features[*ProtoStart];
    X1 = F1->Params[PicoFeatX];
    Y1 = F1->Params[PicoFeatY];
    A1 = F1->Params[PicoFeatDir];

    for (ProtoEnd = ProtoStart + 1,
         SegmentLength = GetPicoFeatureLength();
         ProtoEnd < LastBad;
         ProtoEnd++, SegmentLength += GetPicoFeatureLength()) {
      F2 = Features->Features[*ProtoEnd];
      X2 = F2->Params[PicoFeatX];
      Y2 = F2->Params[PicoFeatY];
      A2 = F2->Params[PicoFeatDir];

      AngleDelta = fabs(A1 - A2);
      if (AngleDelta > 0.5)
        AngleDelta = 1.0 - AngleDelta;

      if (AngleDelta > matcher_clustering_max_angle_delta ||
          fabs(X1 - X2) > SegmentLength ||
          fabs(Y1 - Y2) > SegmentLength)
        break;
    }

    F2 = Features->Features[*(ProtoEnd - 1)];
    X2 = F2->Params[PicoFeatX];
    Y2 = F2->Params[PicoFeatY];
    A2 = F2->Params[PicoFeatDir];

    Pid = AddIntProto(IClass);
    if (Pid == NO_PROTO)
      return (NO_PROTO);

    TempProto = NewTempProto();
    Proto = &(TempProto->Proto);

    /* compute proto params - NOTE that Y_DIM_OFFSET must be used because
       ConvertProto assumes that the Y dimension varies from -0.5 to 0.5
       instead of the -0.25 to 0.75 used in baseline normalization */
    Proto->Length = SegmentLength;
    Proto->Angle = A1;
    Proto->X = (X1 + X2) / 2.0;
    Proto->Y = (Y1 + Y2) / 2.0 - Y_DIM_OFFSET;
    FillABC(Proto);

    TempProto->ProtoId = Pid;
    SET_BIT(TempProtoMask, Pid);

    ConvertProto(Proto, Pid, IClass);
    AddProtoToProtoPruner(Proto, Pid, IClass,
                          classify_learning_debug_level >= 2);

    Class->TempProtos = push(Class->TempProtos, TempProto);
  }
  return IClass->NumProtos - 1;
}                              /* MakeNewTempProtos */

/*---------------------------------------------------------------------------*/
/**
 *
 * @param Templates current set of adaptive templates
 * @param ClassId class containing config to be made permanent
 * @param ConfigId config to be made permanent
 * @param denorm normalization/denormalization parameters
 * @param Blob current blob being adapted to
 *
 * Globals: none
 *
 * @note Exceptions: none
 * @note History: Thu Mar 14 15:54:08 1991, DSJ, Created.
 */
void Classify::MakePermanent(ADAPT_TEMPLATES Templates,
                             CLASS_ID ClassId,
                             int ConfigId,
                             const DENORM& denorm,
                             TBLOB *Blob) {
  UNICHAR_ID *Ambigs;
  TEMP_CONFIG Config;
  ADAPT_CLASS Class;
  PROTO_KEY ProtoKey;

  Class = Templates->Class[ClassId];
  Config = TempConfigFor(Class, ConfigId);

  MakeConfigPermanent(Class, ConfigId);
  if (Class->NumPermConfigs == 0)
    Templates->NumPermClasses++;
  Class->NumPermConfigs++;

  // Initialize permanent config.
  Ambigs = GetAmbiguities(Blob, denorm, ClassId);
  PERM_CONFIG Perm = (PERM_CONFIG) alloc_struct(sizeof(PERM_CONFIG_STRUCT),
                                                "PERM_CONFIG_STRUCT");
  Perm->Ambigs = Ambigs;
  Perm->FontinfoId = Config->FontinfoId;

  // Free memory associated with temporary config (since ADAPTED_CONFIG
  // is a union we need to clean up before we record permanent config).
  ProtoKey.Templates = Templates;
  ProtoKey.ClassId = ClassId;
  ProtoKey.ConfigId = ConfigId;
  Class->TempProtos = delete_d(Class->TempProtos, &ProtoKey, MakeTempProtoPerm);
  FreeTempConfig(Config);

  // Record permanent config.
  PermConfigFor(Class, ConfigId) = Perm;

  if (classify_learning_debug_level >= 1) {
    tprintf("Making config %d for %s (ClassId %d) permanent:"
            " fontinfo id %d, ambiguities '",
            ConfigId, getDict().getUnicharset().debug_str(ClassId).string(),
            ClassId, PermConfigFor(Class, ConfigId)->FontinfoId);
    for (UNICHAR_ID *AmbigsPointer = Ambigs;
        *AmbigsPointer >= 0; ++AmbigsPointer)
      tprintf("%s", unicharset.id_to_unichar(*AmbigsPointer));
    tprintf("'.\n");
  }
}                              /* MakePermanent */
}  // namespace tesseract

/*---------------------------------------------------------------------------*/
/**
 * This routine converts TempProto to be permanent if
 * its proto id is used by the configuration specified in
 * ProtoKey.
 *
 * @param item1 (TEMP_PROTO) temporary proto to compare to key
 * @param item2 (PROTO_KEY) defines which protos to make permanent
 *
 * Globals: none
 *
 * @return TRUE if TempProto is converted, FALSE otherwise
 * @note Exceptions: none
 * @note History: Thu Mar 14 18:49:54 1991, DSJ, Created.
 */
int MakeTempProtoPerm(void *item1, void *item2) {
  ADAPT_CLASS Class;
  TEMP_CONFIG Config;
  TEMP_PROTO TempProto;
  PROTO_KEY *ProtoKey;

  TempProto = (TEMP_PROTO) item1;
  ProtoKey = (PROTO_KEY *) item2;

  Class = ProtoKey->Templates->Class[ProtoKey->ClassId];
  Config = TempConfigFor(Class, ProtoKey->ConfigId);

  if (TempProto->ProtoId > Config->MaxProtoId ||
      !test_bit (Config->Protos, TempProto->ProtoId))
    return FALSE;

  MakeProtoPermanent(Class, TempProto->ProtoId);
  AddProtoToClassPruner(&(TempProto->Proto), ProtoKey->ClassId,
                         ProtoKey->Templates->Templates);
  FreeTempProto(TempProto);

  return TRUE;
}                              /* MakeTempProtoPerm */

/*---------------------------------------------------------------------------*/
namespace tesseract {
/**
 * This routine writes the matches in Results to File.
 *
 * @param File open text file to write Results to
 * @param Results match results to write to File
 *
 * Globals: none
 *
 * @note Exceptions: none
 * @note History: Mon Mar 18 09:24:53 1991, DSJ, Created.
 */
void Classify::PrintAdaptiveMatchResults(FILE *File, ADAPT_RESULTS *Results) {
  for (int i = 0; i < Results->NumMatches; ++i) {
    tprintf("%s(%d), shape %d, %.2f  ",
            unicharset.debug_str(Results->match[i].unichar_id).string(),
            Results->match[i].unichar_id, Results->match[i].shape_id,
            Results->match[i].rating * 100.0);
  }
  tprintf("\n");
}                              /* PrintAdaptiveMatchResults */

/*---------------------------------------------------------------------------*/
/**
 * This routine steps thru each matching class in Results
 * and removes it from the match list if its rating
 * is worse than the BestRating plus a pad.  In other words,
 * all good matches get moved to the front of the classes
 * array.
 *
 * @param Results contains matches to be filtered
 *
 * Globals:
 * - matcher_bad_match_pad defines a "bad match"
 *
 * @note Exceptions: none
 * @note History: Tue Mar 12 13:51:03 1991, DSJ, Created.
 */
void Classify::RemoveBadMatches(ADAPT_RESULTS *Results) {
  int Next, NextGood;
  FLOAT32 BadMatchThreshold;
  static const char* romans = "i v x I V X";
  BadMatchThreshold = Results->best_match.rating + matcher_bad_match_pad;

  if (classify_bln_numeric_mode) {
    UNICHAR_ID unichar_id_one = unicharset.contains_unichar("1") ?
        unicharset.unichar_to_id("1") : -1;
    UNICHAR_ID unichar_id_zero = unicharset.contains_unichar("0") ?
        unicharset.unichar_to_id("0") : -1;
    ScoredClass scored_one = ScoredUnichar(Results, unichar_id_one);
    ScoredClass scored_zero = ScoredUnichar(Results, unichar_id_zero);

    for (Next = NextGood = 0; Next < Results->NumMatches; Next++) {
      if (Results->match[Next].rating <= BadMatchThreshold) {
        ScoredClass match = Results->match[Next];
        if (!unicharset.get_isalpha(match.unichar_id) ||
            strstr(romans,
                   unicharset.id_to_unichar(match.unichar_id)) != NULL) {
          Results->match[NextGood++] = Results->match[Next];
        } else if (unicharset.eq(match.unichar_id, "l") &&
                   scored_one.rating >= BadMatchThreshold) {
          Results->match[NextGood] = scored_one;
          Results->match[NextGood].rating = match.rating;
          NextGood++;
        } else if (unicharset.eq(match.unichar_id, "O") &&
                   scored_zero.rating >= BadMatchThreshold) {
          Results->match[NextGood] = scored_zero;
          Results->match[NextGood].rating = match.rating;
          NextGood++;
        }
      }
    }
  } else {
    for (Next = NextGood = 0; Next < Results->NumMatches; Next++) {
      if (Results->match[Next].rating <= BadMatchThreshold)
        Results->match[NextGood++] = Results->match[Next];
    }
  }
  Results->NumMatches = NextGood;
}                              /* RemoveBadMatches */

/*----------------------------------------------------------------------------*/
/**
 * This routine steps thru each matching class in Results
 * and removes it from the match list if its rating
 * is worse than the BestRating plus a pad.  In other words,
 * all good matches get moved to the front of the classes
 * array.
 *
 * @param Results contains matches to be filtered
 *
 * Globals:
 * - matcher_bad_match_pad defines a "bad match"
 *
 * @note Exceptions: none
 * @note History: Tue Mar 12 13:51:03 1991, DSJ, Created.
 */
void Classify::RemoveExtraPuncs(ADAPT_RESULTS *Results) {
  int Next, NextGood;
  int punc_count;              /*no of garbage characters */
  int digit_count;
  /*garbage characters */
  static char punc_chars[] = ". , ; : / ` ~ ' - = \\ | \" ! _ ^";
  static char digit_chars[] = "0 1 2 3 4 5 6 7 8 9";

  punc_count = 0;
  digit_count = 0;
  for (Next = NextGood = 0; Next < Results->NumMatches; Next++) {
    ScoredClass match = Results->match[Next];
    if (strstr(punc_chars,
               unicharset.id_to_unichar(match.unichar_id)) != NULL) {
      if (punc_count < 2)
        Results->match[NextGood++] = match;
      punc_count++;
    } else {
      if (strstr(digit_chars,
                 unicharset.id_to_unichar(match.unichar_id)) != NULL) {
        if (digit_count < 1)
          Results->match[NextGood++] = match;
        digit_count++;
      } else {
        Results->match[NextGood++] = match;
      }
    }
  }
  Results->NumMatches = NextGood;
}                              /* RemoveExtraPuncs */

/*---------------------------------------------------------------------------*/
/**
 * This routine resets the internal thresholds inside
 * the integer matcher to correspond to the specified
 * threshold.
 *
 * @param Threshold threshold for creating new templates
 *
 * Globals:
 * - matcher_good_threshold default good match rating
 *
 * @note Exceptions: none
 * @note History: Tue Apr  9 08:33:13 1991, DSJ, Created.
 */
void Classify::SetAdaptiveThreshold(FLOAT32 Threshold) {
  Threshold = (Threshold == matcher_good_threshold) ? 0.9: (1.0 - Threshold);
  classify_adapt_proto_threshold.set_value(
      ClipToRange<int>(255 * Threshold, 0, 255));
  classify_adapt_feature_threshold.set_value(
      ClipToRange<int>(255 * Threshold, 0, 255));
}                              /* SetAdaptiveThreshold */

/*---------------------------------------------------------------------------*/
/**
 * This routine compares Blob to both sets of templates
 * (adaptive and pre-trained) and then displays debug
 * information for the config which matched best.
 *
 * @param Blob blob to show best matching config for
 * @param denorm normalization/denormalization parameters
 * @param ClassId class whose configs are to be searched
 * @param shape_id shape index
 * @param AdaptiveOn TRUE if adaptive configs are enabled
 * @param PreTrainedOn TRUE if pretrained configs are enabled
 * @param Results results of match being debugged
 *
 * Globals:
 * - PreTrainedTemplates built-in training
 * - AdaptedTemplates adaptive templates
 * - AllProtosOn dummy proto mask
 * - AllConfigsOn dummy config mask
 *
 * @note Exceptions: none
 * @note History: Fri Mar 22 08:43:52 1991, DSJ, Created.
 */
void Classify::ShowBestMatchFor(TBLOB *Blob,
                                const DENORM& denorm,
                                CLASS_ID ClassId,
                                int shape_id,
                                BOOL8 AdaptiveOn,
                                BOOL8 PreTrainedOn,
                                ADAPT_RESULTS *Results) {
  int NumCNFeatures = 0, NumBLFeatures = 0;
  INT_FEATURE_ARRAY CNFeatures, BLFeatures;
  INT_RESULT_STRUCT CNResult, BLResult;
  inT32 BlobLength;
  uinT32 ConfigMask;
  static int next_config = -1;

  if (PreTrainedOn) next_config = -1;

  CNResult.Rating = BLResult.Rating = 2.0;

  if (!LegalClassId (ClassId)) {
    cprintf ("%d is not a legal class id!!\n", ClassId);
    return;
  }

  uinT8 *CNAdjust = new uinT8[MAX_NUM_CLASSES];
  uinT8 *BLAdjust = new uinT8[MAX_NUM_CLASSES];

  if (shape_table_ == NULL)
    shape_id = ClassId;
  else
    shape_id = ShapeIDToClassID(shape_id);
  if (PreTrainedOn && shape_id >= 0) {
    if (UnusedClassIdIn(PreTrainedTemplates, shape_id)) {
      tprintf("No built-in templates for class/shape %d\n", shape_id);
    } else {
      NumCNFeatures = GetCharNormFeatures(Blob, denorm, PreTrainedTemplates,
                                          CNFeatures, NULL, CNAdjust,
                                          &BlobLength, NULL);
      if (NumCNFeatures <= 0) {
        tprintf("Illegal blob (char norm features)!\n");
      } else {
        im_.SetCharNormMatch(classify_integer_matcher_multiplier);
        im_.Match(ClassForClassId(PreTrainedTemplates, shape_id),
                  AllProtosOn, AllConfigsOn,
                  NumCNFeatures, CNFeatures,
                  &CNResult,
                  classify_adapt_feature_threshold, NO_DEBUG,
                  matcher_debug_separate_windows);
        ExpandShapesAndApplyCorrections(NULL, false, shape_id,
                                        Blob->bounding_box().bottom(),
                                        Blob->bounding_box().top(),
                                        0, BlobLength, CNAdjust,
                                        CNResult, Results);
      }
    }
  }

  if (AdaptiveOn) {
    if (ClassId < 0 || ClassId >= AdaptedTemplates->Templates->NumClasses) {
      tprintf("Invalid adapted class id: %d\n", ClassId);
    } else if (UnusedClassIdIn(AdaptedTemplates->Templates, ClassId) ||
               AdaptedTemplates->Class[ClassId] == NULL ||
               IsEmptyAdaptedClass(AdaptedTemplates->Class[ClassId])) {
      tprintf("No AD templates for class %d = %s\n",
              ClassId, unicharset.id_to_unichar(ClassId));
    } else {
      NumBLFeatures = GetBaselineFeatures(Blob,
                                          denorm,
                                          AdaptedTemplates->Templates,
                                          BLFeatures, BLAdjust,
                                          &BlobLength);
      if (NumBLFeatures <= 0)
        tprintf("Illegal blob (baseline features)!\n");
      else {
        im_.SetBaseLineMatch();
        im_.Match(ClassForClassId(AdaptedTemplates->Templates, ClassId),
                  AllProtosOn, AllConfigsOn,
                  NumBLFeatures, BLFeatures,
                  &BLResult,
                  classify_adapt_feature_threshold, NO_DEBUG,
                  matcher_debug_separate_windows);
        ExpandShapesAndApplyCorrections(
            AdaptedTemplates->Class, false,
            ClassId, Blob->bounding_box().bottom(),
            Blob->bounding_box().top(), 0, BlobLength, CNAdjust,
            BLResult, Results);
      }
    }
  }

  tprintf("\n");
  if (BLResult.Rating < CNResult.Rating) {
    if (next_config < 0) {
      ConfigMask = 1 << BLResult.Config;
      next_config = 0;
    } else {
      ConfigMask = 1 << next_config;
      ++next_config;
    }
    classify_norm_method.set_value(baseline);

    im_.SetBaseLineMatch();
    tprintf("Adaptive Class ID: %d\n", ClassId);
    im_.Match(ClassForClassId(AdaptedTemplates->Templates, ClassId),
              AllProtosOn, (BIT_VECTOR) &ConfigMask,
              NumBLFeatures, BLFeatures,
              &BLResult,
              classify_adapt_feature_threshold,
              matcher_debug_flags,
              matcher_debug_separate_windows);
    ExpandShapesAndApplyCorrections(
        AdaptedTemplates->Class, true,
        ClassId, Blob->bounding_box().bottom(),
        Blob->bounding_box().top(), 0, BlobLength, CNAdjust,
        BLResult, Results);
  } else if (shape_id >= 0) {
    ConfigMask = 1 << CNResult.Config;
    classify_norm_method.set_value(character);

    tprintf("Static Shape ID: %d\n", shape_id);
    im_.SetCharNormMatch(classify_integer_matcher_multiplier);
    im_.Match(ClassForClassId (PreTrainedTemplates, shape_id),
              AllProtosOn, (BIT_VECTOR) & ConfigMask,
              NumCNFeatures, CNFeatures,
              &CNResult,
              classify_adapt_feature_threshold,
              matcher_debug_flags,
              matcher_debug_separate_windows);
    ExpandShapesAndApplyCorrections(NULL, true, shape_id,
                                    Blob->bounding_box().bottom(),
                                    Blob->bounding_box().top(),
                                    0, BlobLength, CNAdjust,
                                    CNResult, Results);
  }

  // Clean up.
  delete[] CNAdjust;
  delete[] BLAdjust;
}                              /* ShowBestMatchFor */

// Returns a string for the classifier class_id: either the corresponding
// unicharset debug_str or the shape_table_ debug str.
STRING Classify::ClassIDToDebugStr(const INT_TEMPLATES_STRUCT* templates,
                                   int class_id, int config_id) const {
  STRING class_string;
  if (templates == PreTrainedTemplates && shape_table_ != NULL) {
    int shape_id = ClassAndConfigIDToFontOrShapeID(class_id, config_id);
    class_string = shape_table_->DebugStr(shape_id);
  } else {
    class_string = unicharset.debug_str(class_id);
  }
  return class_string;
}

// Converts a classifier class_id index to a shape_table_ index
int Classify::ClassAndConfigIDToFontOrShapeID(int class_id,
                                              int int_result_config) const {
  int font_set_id = PreTrainedTemplates->Class[class_id]->font_set_id;
  // Older inttemps have no font_ids.
  if (font_set_id < 0)
    return kBlankFontinfoId;
  const FontSet &fs = fontset_table_.get(font_set_id);
  ASSERT_HOST(int_result_config >= 0 && int_result_config < fs.size);
  return fs.configs[int_result_config];
}

// Converts a shape_table_ index to a classifier class_id index (not a
// unichar-id!). Uses a search, so not fast.
int Classify::ShapeIDToClassID(int shape_id) const {
  for (int id = 0; id < PreTrainedTemplates->NumClasses; ++id) {
    int font_set_id = PreTrainedTemplates->Class[id]->font_set_id;
    ASSERT_HOST(font_set_id >= 0);
    const FontSet &fs = fontset_table_.get(font_set_id);
    for (int config = 0; config < fs.size; ++config) {
      if (fs.configs[config] == shape_id)
        return id;
    }
  }
  tprintf("Shape %d not found\n", shape_id);
  return -1;
}

// Returns true if the given TEMP_CONFIG is good enough to make it
// a permanent config.
bool Classify::TempConfigReliable(CLASS_ID class_id,
                                  const TEMP_CONFIG &config) {
  if (classify_learning_debug_level >= 1) {
    tprintf("NumTimesSeen for config of %s is %d\n",
            getDict().getUnicharset().debug_str(class_id).string(),
            config->NumTimesSeen);
  }
  if (config->NumTimesSeen >= matcher_sufficient_examples_for_prototyping) {
    return true;
  } else if (config->NumTimesSeen < matcher_min_examples_for_prototyping) {
    return false;
  } else if (use_ambigs_for_adaption) {
    // Go through the ambigs vector and see whether we have already seen
    // enough times all the characters represented by the ambigs vector.
    const UnicharIdVector *ambigs =
      getDict().getUnicharAmbigs().AmbigsForAdaption(class_id);
    int ambigs_size = (ambigs == NULL) ? 0 : ambigs->size();
    for (int ambig = 0; ambig < ambigs_size; ++ambig) {
      ADAPT_CLASS ambig_class = AdaptedTemplates->Class[(*ambigs)[ambig]];
      assert(ambig_class != NULL);
      if (ambig_class->NumPermConfigs == 0 &&
          ambig_class->MaxNumTimesSeen <
          matcher_min_examples_for_prototyping) {
        if (classify_learning_debug_level >= 1) {
          tprintf("Ambig %s has not been seen enough times,"
                  " not making config for %s permanent\n",
                  getDict().getUnicharset().debug_str(
                      (*ambigs)[ambig]).string(),
                  getDict().getUnicharset().debug_str(class_id).string());
        }
        return false;
      }
    }
  }
  return true;
}

void Classify::UpdateAmbigsGroup(CLASS_ID class_id, const DENORM& denorm,
                                 TBLOB *Blob) {
  const UnicharIdVector *ambigs =
    getDict().getUnicharAmbigs().ReverseAmbigsForAdaption(class_id);
  int ambigs_size = (ambigs == NULL) ? 0 : ambigs->size();
  if (classify_learning_debug_level >= 1) {
    tprintf("Running UpdateAmbigsGroup for %s class_id=%d\n",
            getDict().getUnicharset().debug_str(class_id).string(), class_id);
  }
  for (int ambig = 0; ambig < ambigs_size; ++ambig) {
    CLASS_ID ambig_class_id = (*ambigs)[ambig];
    const ADAPT_CLASS ambigs_class = AdaptedTemplates->Class[ambig_class_id];
    for (int cfg = 0; cfg < MAX_NUM_CONFIGS; ++cfg) {
      if (ConfigIsPermanent(ambigs_class, cfg)) continue;
      const TEMP_CONFIG config =
        TempConfigFor(AdaptedTemplates->Class[ambig_class_id], cfg);
      if (config != NULL && TempConfigReliable(ambig_class_id, config)) {
        if (classify_learning_debug_level >= 1) {
          tprintf("Making config %d of %s permanent\n", cfg,
                  getDict().getUnicharset().debug_str(
                      ambig_class_id).string());
        }
        MakePermanent(AdaptedTemplates, ambig_class_id, cfg, denorm, Blob);
      }
    }
  }
}

}  // namespace tesseract
