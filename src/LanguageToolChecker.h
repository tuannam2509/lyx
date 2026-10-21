// -*- C++ -*-
/**
 * \file LanguageToolChecker.h
 * This file is part of LyX, the document processor.
 * Licence details can be found in the file COPYING.
 *
 * Spelling and grammar checking with a LanguageTool server
 * (https://languagetool.org), using its HTTP API (v2).
 *
 * Full author contact details are available in file CREDITS.
 */

#ifndef LYX_LANGUAGETOOL_CHECKER_H
#define LYX_LANGUAGETOOL_CHECKER_H

#include "SpellChecker.h"

#include <functional>

namespace lyx {

/// A spell and grammar checker that asks a LanguageTool server.
/**
 * Paragraphs are checked as a whole, so that grammar and style
 * problems can be found as well. The server is queried asynchronously:
 * while typing, a paragraph is checked once the user pauses. When the
 * answer arrives, the screen is redrawn with the results (via the
 * redraw hook installed by the frontend).
 */
class LanguageToolChecker : public SpellChecker
{
public:
	LanguageToolChecker();
	~LanguageToolChecker();

	/// \name SpellChecker inherited methods.
	///@{
	enum Result check(WordLangTuple const &,
			  std::vector<WordLangTuple> const &) override;
	void suggest(WordLangTuple const &, docstring_list &) override;
	void stem(WordLangTuple const &, docstring_list &) override {}
	void insert(WordLangTuple const &) override;
	void remove(WordLangTuple const &) override;
	void accept(WordLangTuple const &) override;
	bool hasDictionary(Language const * lang) const override;
	int numDictionaries() const override;
	bool canCheckParagraph() const override { return true; }
	bool checksWholeParagraph() const override { return true; }
	Result checkSegments(std::vector<TextSegment> const &,
			     Language const *) override;
	int numMisspelledWords() const override;
	void misspelledWord(int index, int & start, int & length) const override;
	Result misspelledWordResult(int index) const override;
	docstring explanation(WordLangTuple const &) const override;
	void setBlocking(bool) override;
	docstring const error() override;
	void advanceChangeNumber() override;
	///@}

	/// The frontend installs a function here which redraws the
	/// work areas when new results have arrived.
	static void setRedrawHook(std::function<void()> const & hook);

private:
	struct Private;
	Private * d;
};


} // namespace lyx

#endif // LYX_LANGUAGETOOL_CHECKER_H
