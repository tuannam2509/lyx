/**
 * \file LanguageToolChecker.cpp
 * This file is part of LyX, the document processor.
 * Licence details can be found in the file COPYING.
 *
 * Full author contact details are available in file CREDITS.
 */

#include <config.h>

#include "LanguageToolChecker.h"

#include "FuncCode.h"
#include "FuncRequest.h"
#include "Language.h"
#include "LyX.h"
#include "LyXRC.h"
#include "PersonalWordList.h"
#include "WordLangTuple.h"

#include "support/debug.h"
#include "support/docstring_list.h"
#include "support/gettext.h"
#include "support/lstrings.h"
#include "support/qstring_helpers.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <deque>
#include <map>
#include <set>

using namespace std;
using namespace lyx::support;

namespace lyx {

namespace {

/// how long to wait after the last change before asking the server [ms]
int const debounce_delay = 700;
/// how long new requests are sent without delay after a pause [ms]
int const fetch_window = 1500;
/// after a connection failure, wait this long before trying again [ms]
int const offline_delay = 15000;
/// timeout of a blocking request [ms]
int const blocking_timeout = 15000;
/// maximal number of cached paragraph results
size_t const max_cache = 4000;


std::function<void()> & redrawHook()
{
	static std::function<void()> hook;
	return hook;
}


/// One problem found by LanguageTool
struct Issue {
	/// start (in characters of the checked text)
	int start = 0;
	/// length (in characters)
	int length = 0;
	/// grammar, style, typography... (rather than spelling)
	bool grammar = false;
	///
	docstring message;
	///
	docstring_list replacements;
	///
	string rule;
};

typedef vector<Issue> Issues;


/// Base URL of the API, normalized to end with "/v2"
QString apiUrl()
{
	QString url = toqstr(lyxrc.languagetool_url).trimmed();
	while (url.endsWith('/'))
		url.chop(1);
	if (url.isEmpty())
		url = "http://localhost:8081";
	if (!url.endsWith("/v2"))
		url += "/v2";
	return url;
}


/// Run a blocking POST in a separate thread, which has its own event loop.
/// The calling (GUI) thread waits, which avoids any re-entrance.
class BlockingPost : public QThread
{
public:
	BlockingPost(QNetworkRequest const & req, QByteArray const & body)
		: req_(req), body_(body), ok_(false) {}
	void run() override
	{
		QNetworkAccessManager nam;
		QEventLoop loop;
		QNetworkReply * reply = nam.post(req_, body_);
		QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		QTimer::singleShot(blocking_timeout, &loop, &QEventLoop::quit);
		loop.exec();
		if (reply->isFinished()) {
			ok_ = reply->error() == QNetworkReply::NoError;
			data_ = reply->readAll();
			error_ = reply->errorString();
			status_ = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		} else {
			reply->abort();
			error_ = "timeout";
		}
		delete reply;
	}
	QNetworkRequest req_;
	QByteArray body_;
	QByteArray data_;
	QString error_;
	int status_ = 0;
	bool ok_;
};

} // namespace


struct LanguageToolChecker::Private
{
	explicit Private(LanguageToolChecker * owner) : owner_(owner)
	{
		clock_.start();
	}

	~Private()
	{
		for (auto & p : personal_) {
			if (p.second && p.second->isDirty())
				p.second->save();
			delete p.second;
		}
		delete context_;
	}

	/// make sure the Qt objects exist (they need a QCoreApplication)
	bool ensureNetwork();
	/// ask the server which languages it supports
	void fetchLanguages();
	/// LanguageTool code of a LyX language; empty if not supported
	QString ltCode(Language const * lang) const;
	/// build the request for a text
	QNetworkRequest request(QString const & endpoint) const;
	QByteArray body(QString const & code, vector<TextSegment> const & segs) const;
	/// key of the cache
	static string cacheKey(QString const & code, vector<TextSegment> const & segs);
	/// send a request now
	void send(string const & key, QString const & code, QByteArray const & body);
	/// handle a reply; returns false on connection problems
	bool handleReply(string const & key, QString const & code,
	                 QString const & text16,
	                 QByteArray const & data, bool ok, int status,
	                 QString const & errstr);
	/// parse the JSON reply of the server
	Issues parse(QByteArray const & data, QString const & text16) const;
	/// store in cache
	void store(string const & key, Issues const & issues);
	/// request a (coalesced) redraw of the screen
	void scheduleRedraw();
	/// the pause after typing is over: check what is visible now
	void startFetching();
	/// personal word list for language
	PersonalWordList * personal(Language const * lang);
	///
	bool isIgnored(WordLangTuple const & wl) const;
	///
	bool isLearned(WordLangTuple const & wl);
	///
	void setError(docstring const & err);

	LanguageToolChecker * owner_;
	/// parent of the Qt objects below
	QObject * context_ = nullptr;
	QNetworkAccessManager * nam_ = nullptr;
	QTimer * debounce_ = nullptr;
	QElapsedTimer clock_;
	qint64 fetch_until_ = -1;
	qint64 offline_until_ = -1;
	bool redraw_pending_ = false;
	bool blocking_ = false;

	/// results per checked text
	map<string, Issues> cache_;
	deque<string> cache_order_;
	/// waiting for the end of the typing pause: key -> (language, body)
	map<string, pair<QString, QByteArray>> pending_;
	/// UTF-16 text of the pending/in-flight requests
	map<string, QString> texts_;
	/// sent, but not answered yet
	set<string> inflight_;

	/// issues of the last check, after filtering
	Issues current_;
	/// the last check text (UCS-4)
	docstring current_text_;
	/// issues by flagged phrase (language + text), for suggestions
	map<string, Issue> phrases_;

	/// personal word lists
	map<string, PersonalWordList *> personal_;
	/// words ignored for this session
	vector<WordLangTuple> ignored_;

	/// languages supported by the server (codes and long codes)
	QSet<QString> server_langs_;
	bool langs_requested_ = false;
	/// languages rejected by the server
	QSet<QString> rejected_;

	docstring error_;
};


bool LanguageToolChecker::Private::ensureNetwork()
{
	if (nam_)
		return true;
	if (!QCoreApplication::instance())
		return false;
	context_ = new QObject;
	nam_ = new QNetworkAccessManager(context_);
	debounce_ = new QTimer(context_);
	debounce_->setSingleShot(true);
	debounce_->setInterval(debounce_delay);
	QObject::connect(debounce_, &QTimer::timeout, context_,
	                 [this]() { startFetching(); });
	fetchLanguages();
	return true;
}


QNetworkRequest LanguageToolChecker::Private::request(QString const & endpoint) const
{
	QNetworkRequest req(QUrl(apiUrl() + endpoint));
	req.setHeader(QNetworkRequest::ContentTypeHeader,
	              "application/x-www-form-urlencoded; charset=utf-8");
	req.setRawHeader("Accept", "application/json");
	req.setRawHeader("User-Agent", "LyX");
	return req;
}


void LanguageToolChecker::Private::fetchLanguages()
{
	if (!nam_ || langs_requested_)
		return;
	langs_requested_ = true;
	QNetworkReply * reply = nam_->get(request("/languages"));
	QObject::connect(reply, &QNetworkReply::finished, context_, [this, reply]() {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			LYXERR(Debug::GUI, "LanguageTool: cannot read languages: "
			       << fromqstr(reply->errorString()));
			// try again later
			langs_requested_ = false;
			return;
		}
		QJsonArray const langs = QJsonDocument::fromJson(reply->readAll()).array();
		server_langs_.clear();
		for (QJsonValue const & v : langs) {
			QJsonObject const o = v.toObject();
			server_langs_.insert(o.value("longCode").toString());
			server_langs_.insert(o.value("code").toString());
		}
		LYXERR(Debug::GUI, "LanguageTool: server supports "
		       << server_langs_.size() << " language codes");
		// results may change for languages that were unknown so far
		owner_->nextChangeNumber();
		scheduleRedraw();
	});
}


QString LanguageToolChecker::Private::ltCode(Language const * lang) const
{
	if (!lang)
		return QString();
	QString full = toqstr(lang->code());
	full.replace('_', '-');
	if (full.isEmpty())
		return QString();
	QString const prefix = full.section('-', 0, 0);
	if (server_langs_.isEmpty()) {
		// we do not know (yet), just try
		if (rejected_.contains(full))
			return rejected_.contains(prefix) ? QString() : prefix;
		return full;
	}
	if (server_langs_.contains(full) && !rejected_.contains(full))
		return full;
	if (server_langs_.contains(prefix))
		return prefix;
	for (QString const & c : server_langs_)
		if (c.startsWith(prefix + '-'))
			return c;
	return QString();
}


string LanguageToolChecker::Private::cacheKey(QString const & code,
		vector<TextSegment> const & segs)
{
	docstring key = qstring_to_ucs4(code);
	key += char_type(lyxrc.languagetool_picky ? 'p' : 'n');
	for (auto const & s : segs) {
		if (s.markup) {
			key += char_type(1);
			key += s.interpret_as;
			key += char_type(2);
		}
		key += s.text;
	}
	return to_utf8(key);
}


QByteArray LanguageToolChecker::Private::body(QString const & code,
		vector<TextSegment> const & segs) const
{
	QJsonArray annotation;
	for (auto const & s : segs) {
		QJsonObject o;
		if (s.markup) {
			o.insert("markup", toqstr(s.text));
			o.insert("interpretAs", toqstr(s.interpret_as));
		} else
			o.insert("text", toqstr(s.text));
		annotation.append(o);
	}
	QJsonObject data;
	data.insert("annotation", annotation);
	QUrlQuery q;
	q.addQueryItem("language", code);
	q.addQueryItem("data", QString::fromUtf8(
		QJsonDocument(data).toJson(QJsonDocument::Compact)));
	if (lyxrc.languagetool_picky)
		q.addQueryItem("level", "picky");
	// make sure that '+' and '&' etc. are encoded in the form data
	QByteArray out;
	for (auto const & item : q.queryItems(QUrl::FullyDecoded)) {
		if (!out.isEmpty())
			out += '&';
		out += QUrl::toPercentEncoding(item.first) + '='
			+ QUrl::toPercentEncoding(item.second);
	}
	return out;
}


Issues LanguageToolChecker::Private::parse(QByteArray const & data,
		QString const & text16) const
{
	Issues issues;
	// map UTF-16 offsets (used by LanguageTool) to UCS-4 offsets
	vector<int> map16;
	map16.reserve(text16.size() + 1);
	int ucs4 = 0;
	for (int i = 0; i < text16.size(); ++i) {
		map16.push_back(ucs4);
		if (!(text16.at(i).isHighSurrogate() && i + 1 < text16.size()
		      && text16.at(i + 1).isLowSurrogate()))
			++ucs4;
	}
	map16.push_back(ucs4);
	auto toUcs4 = [&map16](int off) {
		if (off < 0)
			return 0;
		if (off >= int(map16.size()))
			return map16.back();
		return map16[off];
	};

	QJsonObject const root = QJsonDocument::fromJson(data).object();
	QJsonArray const matches = root.value("matches").toArray();
	for (QJsonValue const & v : matches) {
		QJsonObject const m = v.toObject();
		int const off = m.value("offset").toInt();
		int const len = m.value("length").toInt();
		Issue is;
		is.start = toUcs4(off);
		is.length = toUcs4(off + len) - is.start;
		if (is.length <= 0)
			continue;
		QJsonObject const rule = m.value("rule").toObject();
		QString const type = rule.value("issueType").toString();
		QString const cat = rule.value("category").toObject().value("id").toString();
		is.grammar = type != "misspelling" && cat != "TYPOS";
		is.rule = fromqstr(rule.value("id").toString());
		QString msg = m.value("message").toString();
		if (msg.isEmpty())
			msg = m.value("shortMessage").toString();
		is.message = qstring_to_ucs4(msg);
		QJsonArray const repl = m.value("replacements").toArray();
		for (QJsonValue const & r : repl) {
			if (is.replacements.size() >= 12)
				break;
			is.replacements.push_back(qstring_to_ucs4(r.toObject().value("value").toString()));
		}
		issues.push_back(is);
	}
	return issues;
}


void LanguageToolChecker::Private::store(string const & key, Issues const & issues)
{
	if (cache_.find(key) == cache_.end())
		cache_order_.push_back(key);
	cache_[key] = issues;
	while (cache_.size() > max_cache && !cache_order_.empty()) {
		cache_.erase(cache_order_.front());
		cache_order_.pop_front();
	}
}


void LanguageToolChecker::Private::setError(docstring const & err)
{
	bool const changed = err != error_;
	error_ = err;
	if (changed && !err.empty()) {
		LYXERR0("LanguageTool: " << to_utf8(err));
		dispatch(FuncRequest(LFUN_MESSAGE, err));
	}
}


bool LanguageToolChecker::Private::handleReply(string const & key,
		QString const & code, QString const & text16, QByteArray const & data, bool ok,
		int status, QString const & errstr)
{
	if (ok) {
		store(key, parse(data, text16));
		error_.clear();
		return true;
	}
	if (status >= 400 && status < 500) {
		// the server understood, but did not like the request
		// (typically: unsupported language). Do not ask again.
		QString const answer = QString::fromUtf8(data).left(200);
		LYXERR(Debug::GUI, "LanguageTool rejected request: " << fromqstr(answer));
		if (answer.contains("language", Qt::CaseInsensitive))
			// remember this language as not supported
			rejected_.insert(code);
		store(key, Issues());
		return true;
	}
	setError(bformat(_("LanguageTool server not reachable at %1$s: %2$s"),
	                 qstring_to_ucs4(apiUrl()), qstring_to_ucs4(errstr)));
	offline_until_ = clock_.elapsed() + offline_delay;
	return false;
}


void LanguageToolChecker::Private::send(string const & key, QString const & code,
		QByteArray const & bdy)
{
	if (!ensureNetwork() || inflight_.count(key))
		return;
	inflight_.insert(key);
	QNetworkReply * reply = nam_->post(request("/check"), bdy);
	QObject::connect(reply, &QNetworkReply::finished, context_,
	                 [this, reply, key, code]() {
		reply->deleteLater();
		inflight_.erase(key);
		QString const text16 = texts_[key];
		texts_.erase(key);
		bool const ok = reply->error() == QNetworkReply::NoError;
		int const status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (handleReply(key, code, text16, reply->readAll(), ok, status,
		                reply->errorString())) {
			owner_->nextChangeNumber();
			scheduleRedraw();
		}
	});
}


void LanguageToolChecker::Private::scheduleRedraw()
{
	if (redraw_pending_ || !context_)
		return;
	redraw_pending_ = true;
	// coalesce the answers of several requests into one redraw
	QTimer::singleShot(60, context_, [this]() {
		redraw_pending_ = false;
		// do not fetch new things during this redraw, unless we
		// are in the fetching phase anyway
		if (redrawHook())
			redrawHook()();
	});
}


void LanguageToolChecker::Private::startFetching()
{
	// The user paused. Forget the intermediate states of the text and
	// redraw everything, so that the paragraphs that are visible now are
	// requested immediately (cache hits cost nothing).
	for (auto const & p : pending_)
		if (!inflight_.count(p.first))
			texts_.erase(p.first);
	pending_.clear();
	fetch_until_ = clock_.elapsed() + fetch_window;
	owner_->nextChangeNumber();
	if (redrawHook())
		redrawHook()();
}


PersonalWordList * LanguageToolChecker::Private::personal(Language const * lang)
{
	if (!lang)
		return nullptr;
	PersonalWordList *& pd = personal_[lang->lang()];
	if (!pd) {
		pd = new PersonalWordList(lang->lang());
		pd->load();
	}
	return pd;
}


bool LanguageToolChecker::Private::isIgnored(WordLangTuple const & wl) const
{
	for (auto const & w : ignored_)
		if (w.lang() && wl.lang() && w.lang()->code() == wl.lang()->code()
		    && w.word() == wl.word())
			return true;
	return false;
}


bool LanguageToolChecker::Private::isLearned(WordLangTuple const & wl)
{
	PersonalWordList * pd = personal(wl.lang());
	return pd && pd->exists(wl.word());
}


LanguageToolChecker::LanguageToolChecker()
	: d(new Private(this))
{
}


LanguageToolChecker::~LanguageToolChecker()
{
	delete d;
}


void LanguageToolChecker::setRedrawHook(std::function<void()> const & hook)
{
	redrawHook() = hook;
}


SpellChecker::Result LanguageToolChecker::checkSegments(
		vector<TextSegment> const & segs, Language const * lang)
{
	d->current_.clear();
	d->current_text_.clear();
	for (auto const & s : segs)
		d->current_text_ += s.text;
	if (d->current_text_.empty())
		return WORD_OK;
	if (!d->ensureNetwork())
		return WORD_OK;
	QString const code = d->ltCode(lang);
	if (code.isEmpty())
		return NO_DICTIONARY;

	string const key = d->cacheKey(code, segs);
	auto it = d->cache_.find(key);
	LYXERR(Debug::GUI, "LanguageTool: check [" << fromqstr(code) << "] "
	       << (it == d->cache_.end() ? "miss" : "hit") << ": "
	       << to_utf8(d->current_text_.substr(0, 60)));
	if (it == d->cache_.end()) {
		// not known yet: ask the server
		QByteArray const bdy = d->body(code, segs);
		d->texts_[key] = toqstr(d->current_text_);
		qint64 const now = d->clock_.elapsed();
		if (d->blocking_) {
			BlockingPost post(d->request("/check"), bdy);
			post.start();
			post.wait();
			d->handleReply(key, code, d->texts_[key], post.data_, post.ok_,
			               post.status_, post.error_);
			d->texts_.erase(key);
			it = d->cache_.find(key);
		} else if (now < d->offline_until_) {
			d->texts_.erase(key);
			return WORD_OK;
		} else if (now < d->fetch_until_) {
			d->send(key, code, bdy);
		} else {
			// typing: wait for a pause
			d->pending_[key] = make_pair(code, bdy);
			d->debounce_->start();
		}
		if (it == d->cache_.end())
			return WORD_OK;
	}

	Result result = WORD_OK;
	for (Issue const & is : it->second) {
		if (is.start < 0 || is.start + is.length > int(d->current_text_.size()))
			continue;
		docstring const phrase = d->current_text_.substr(is.start, is.length);
		WordLangTuple const wl(phrase, lang);
		if (!is.grammar && (d->isLearned(wl) || d->isIgnored(wl)))
			continue;
		d->current_.push_back(is);
		string const pkey = lang->lang() + '\x1f' + to_utf8(phrase);
		d->phrases_[pkey] = is;
		if (!is.grammar)
			result = UNKNOWN_WORD;
		else if (result == WORD_OK)
			result = GRAMMAR_ERROR;
	}
	if (d->phrases_.size() > 20000)
		d->phrases_.clear();
	return result;
}


SpellChecker::Result LanguageToolChecker::check(WordLangTuple const & wl,
		vector<WordLangTuple> const & /*docdict*/)
{
	// A single word: look it up in the results of the paragraph checks
	string const pkey = (wl.lang() ? wl.lang()->lang() : string())
		+ '\x1f' + to_utf8(wl.word());
	auto it = d->phrases_.find(pkey);
	d->current_.clear();
	d->current_text_ = wl.word();
	if (d->isLearned(wl))
		return LEARNED_WORD;
	if (d->isIgnored(wl))
		return IGNORED_WORD;
	if (it == d->phrases_.end())
		return WORD_OK;
	Issue is = it->second;
	is.start = 0;
	is.length = int(wl.word().size());
	d->current_.push_back(is);
	return is.grammar ? GRAMMAR_ERROR : UNKNOWN_WORD;
}


void LanguageToolChecker::suggest(WordLangTuple const & wl,
		docstring_list & suggestions)
{
	suggestions.clear();
	string const pkey = (wl.lang() ? wl.lang()->lang() : string())
		+ '\x1f' + to_utf8(wl.word());
	auto it = d->phrases_.find(pkey);
	if (it != d->phrases_.end())
		suggestions = it->second.replacements;
}


docstring LanguageToolChecker::explanation(WordLangTuple const & wl) const
{
	string const pkey = (wl.lang() ? wl.lang()->lang() : string())
		+ '\x1f' + to_utf8(wl.word());
	auto it = d->phrases_.find(pkey);
	if (it != d->phrases_.end())
		return it->second.message;
	return docstring();
}


void LanguageToolChecker::insert(WordLangTuple const & wl)
{
	if (PersonalWordList * pd = d->personal(wl.lang()))
		pd->insert(wl.word());
	nextChangeNumber();
}


void LanguageToolChecker::remove(WordLangTuple const & wl)
{
	if (PersonalWordList * pd = d->personal(wl.lang()))
		pd->remove(wl.word());
	nextChangeNumber();
}


void LanguageToolChecker::accept(WordLangTuple const & wl)
{
	d->ignored_.push_back(wl);
	nextChangeNumber();
}


bool LanguageToolChecker::hasDictionary(Language const * lang) const
{
	return !d->ltCode(lang).isEmpty();
}


int LanguageToolChecker::numDictionaries() const
{
	// unknown until the server answered; assume it works
	return d->server_langs_.isEmpty() ? 1 : d->server_langs_.size();
}


int LanguageToolChecker::numMisspelledWords() const
{
	return int(d->current_.size());
}


void LanguageToolChecker::misspelledWord(int index, int & start, int & length) const
{
	if (index < 0 || index >= int(d->current_.size())) {
		start = 0;
		length = 0;
		return;
	}
	start = d->current_[index].start;
	length = d->current_[index].length;
}


SpellChecker::Result LanguageToolChecker::misspelledWordResult(int index) const
{
	if (index < 0 || index >= int(d->current_.size()))
		return UNKNOWN_WORD;
	return d->current_[index].grammar ? GRAMMAR_ERROR : UNKNOWN_WORD;
}


void LanguageToolChecker::setBlocking(bool block)
{
	if (block && !d->blocking_)
		// recheck everything, waiting for the results
		nextChangeNumber();
	d->blocking_ = block;
}


docstring const LanguageToolChecker::error()
{
	return d->error_;
}


void LanguageToolChecker::advanceChangeNumber()
{
	// settings (server, level) may have changed: forget everything
	d->cache_.clear();
	d->cache_order_.clear();
	d->pending_.clear();
	d->phrases_.clear();
	d->server_langs_.clear();
	d->rejected_.clear();
	d->langs_requested_ = false;
	d->offline_until_ = -1;
	d->error_.clear();
	d->fetchLanguages();
	nextChangeNumber();
}


} // namespace lyx
