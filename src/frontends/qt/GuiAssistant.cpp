/**
 * \file GuiAssistant.cpp
 * This file is part of LyX, the document processor.
 * Licence details can be found in the file COPYING.
 *
 * Full author contact details are available in file CREDITS.
 */

#include <config.h>

#include "GuiAssistant.h"

#include "GuiApplication.h"
#include "GuiView.h"
#include "qt_helpers.h"

#include "Buffer.h"
#include "BufferParams.h"
#include "BufferView.h"
#include "Cursor.h"
#include "FuncRequest.h"
#include "InsetList.h"
#include "Language.h"
#include "LyXRC.h"
#include "Paragraph.h"
#include "ParagraphList.h"
#include "Text.h"
#include "update_flags.h"

#include "support/debug.h"
#include "support/gettext.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDate>
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

using namespace std;

namespace lyx {
namespace frontend {

namespace {

/// maximal number of tool rounds per question
int const max_rounds = 5;
/// maximal size of the document text sent as context (characters)
int const max_context = 60000;
/// maximal size of a fetched web page given to the model (characters)
int const max_page = 12000;


struct QuickAction {
	char const * label;
	char const * prompt;
	/// the answer is a new version of the context (offer "Replace")
	bool rewrite;
};


QuickAction const quick_actions[] = {
	{ N_("Proofread (spelling & grammar)"),
	  "Correct the spelling, grammar and punctuation of the text. Keep the "
	  "meaning, the tone and the wording otherwise. Reply with the corrected "
	  "text only.", true },
	{ N_("Explain the mistakes"),
	  "List the spelling, grammar and style problems of the text as a short "
	  "bulleted list. For each item quote the problem and give the fix.", false },
	{ N_("Improve clarity and flow"),
	  "Rewrite the text so that it reads more clearly and flows better. Keep "
	  "the meaning and the voice of the author. Reply with the revised text "
	  "only.", true },
	{ N_("Make it more concise"),
	  "Make the text more concise without losing information. Reply with the "
	  "revised text only.", true },
	{ N_("Expand with more detail"),
	  "Expand the text with more detail and depth, in the same style. Reply "
	  "with the revised text only.", true },
	{ N_("Make it more formal"),
	  "Rewrite the text in a more formal, academic tone. Reply with the revised "
	  "text only.", true },
	{ N_("Make it more friendly"),
	  "Rewrite the text in a warmer, more conversational tone. Reply with the "
	  "revised text only.", true },
	{ N_("Continue writing"),
	  "Continue the text with one or two paragraphs in the same voice and "
	  "style. Reply with the new text only.", false },
	{ N_("Summarize"),
	  "Summarize the text in a few sentences.", false },
	{ N_("Suggest a title"),
	  "Suggest five possible titles or headings for the text.", false },
	{ N_("Translate to English"),
	  "Translate the text to English. Reply with the translation only.", true },
	{ nullptr, nullptr, false }
};


QString context_label(int kind)
{
	switch (kind) {
	case 0: return qt_("Auto: selection or paragraph");
	case 1: return qt_("Selection");
	case 2: return qt_("Current paragraph");
	case 3: return qt_("Whole document");
	default: return qt_("No document context");
	}
}

} // namespace


GuiAssistant::GuiAssistant(GuiView & parent, Qt::DockWidgetArea area,
	Qt::WindowFlags flags)
	: DockView(parent, "assistant", qt_("Writing Assistant"), area, flags),
	  nam_(new QNetworkAccessManager(this)), streamed_(false),
	  reasoning_(false), round_(0), use_tools_(false),
	  tools_unsupported_(false), target_kind_(CONTEXT_NONE),
	  target_par_id_(-1), last_was_rewrite_(false)
{
	panel_ = new QWidget(this);
	panel_->setObjectName("assistantPanel");
	QVBoxLayout * top = new QVBoxLayout(panel_);
	top->setContentsMargins(8, 8, 8, 8);
	top->setSpacing(6);

	// model selection and new chat
	QHBoxLayout * header = new QHBoxLayout;
	modelCO_ = new QComboBox(panel_);
	modelCO_->setEditable(true);
	modelCO_->setToolTip(qt_("The model to talk to"));
	modelCO_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	modelCO_->setMinimumContentsLength(8);
	modelCO_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	newChatTB_ = new QToolButton(panel_);
	newChatTB_->setText(qt_("New Chat"));
	newChatTB_->setToolTip(qt_("Forget the conversation and start a new one"));
	header->addWidget(modelCO_, 1);
	header->addWidget(newChatTB_);
	top->addLayout(header);

	// the conversation
	transcript_ = new QTextBrowser(panel_);
	transcript_->setOpenExternalLinks(true);
	transcript_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	top->addWidget(transcript_, 1);

	// what to do with the last answer
	replyBar_ = new QWidget(panel_);
	QHBoxLayout * rl = new QHBoxLayout(replyBar_);
	rl->setContentsMargins(0, 0, 0, 0);
	insertTB_ = new QToolButton(replyBar_);
	insertTB_->setText(qt_("Insert"));
	insertTB_->setToolTip(qt_("Insert the answer at the cursor position (replacing the selection)"));
	replaceTB_ = new QToolButton(replyBar_);
	replaceTB_->setText(qt_("Replace"));
	replaceTB_->setToolTip(qt_("Replace the text the request was about with the answer"));
	copyTB_ = new QToolButton(replyBar_);
	copyTB_->setText(qt_("Copy"));
	copyTB_->setToolTip(qt_("Copy the answer to the clipboard"));
	rl->addWidget(insertTB_);
	rl->addWidget(replaceTB_);
	rl->addWidget(copyTB_);
	rl->addStretch(1);
	top->addWidget(replyBar_);

	// context, web search and quick actions
	QHBoxLayout * options = new QHBoxLayout;
	contextCO_ = new QComboBox(panel_);
	for (int i = 0; i <= CONTEXT_NONE; ++i)
		contextCO_->addItem(context_label(i), i);
	contextCO_->setToolTip(qt_("Which part of the document is sent along with your message"));
	webTB_ = new QToolButton(panel_);
	webTB_->setText(qt_("Web"));
	webTB_->setCheckable(true);
	webTB_->setToolTip(qt_("Allow the assistant to search the web (SearXNG) and read web pages"));
	actionsTB_ = new QToolButton(panel_);
	actionsTB_->setText(qt_("Actions"));
	actionsTB_->setPopupMode(QToolButton::InstantPopup);
	actionsTB_->setToolTip(qt_("Quick actions on the selection or the current paragraph"));
	actionsMenu_ = new QMenu(actionsTB_);
	actionsTB_->setMenu(actionsMenu_);
	buildActionsMenu();
	options->addWidget(contextCO_, 1);
	options->addWidget(webTB_);
	options->addWidget(actionsTB_);
	top->addLayout(options);

	// input
	QHBoxLayout * input = new QHBoxLayout;
	inputTE_ = new QPlainTextEdit(panel_);
	inputTE_->setPlaceholderText(qt_("Ask anything about your text... (Enter to send, Shift+Enter for a new line)"));
	inputTE_->setTabChangesFocus(true);
	inputTE_->setMaximumHeight(110);
	inputTE_->setMinimumHeight(56);
	inputTE_->installEventFilter(this);
	sendPB_ = new QPushButton(qt_("Send"), panel_);
	sendPB_->setObjectName("assistantSendButton");
	input->addWidget(inputTE_, 1);
	input->addWidget(sendPB_, 0, Qt::AlignBottom);
	top->addLayout(input);

	statusLA_ = new QLabel(panel_);
	statusLA_->setObjectName("assistantStatus");
	statusLA_->setWordWrap(true);
	top->addWidget(statusLA_);

	setWidget(panel_);
	setMinimumWidth(260);

	renderTimer_ = new QTimer(this);
	renderTimer_->setSingleShot(true);
	renderTimer_->setInterval(80);

	connect(sendPB_, SIGNAL(clicked()), this, SLOT(send()));
	connect(newChatTB_, SIGNAL(clicked()), this, SLOT(newChat()));
	connect(insertTB_, SIGNAL(clicked()), this, SLOT(insertReply()));
	connect(replaceTB_, SIGNAL(clicked()), this, SLOT(replaceWithReply()));
	connect(copyTB_, SIGNAL(clicked()), this, SLOT(copyReply()));
	connect(renderTimer_, SIGNAL(timeout()), this, SLOT(render()));

	QSettings settings;
	webTB_->setChecked(settings.value("assistant/websearch", false).toBool());
	contextCO_->setCurrentIndex(settings.value("assistant/context", 0).toInt());
	QString model = toqstr(lyxrc.ai_model);
	if (model.isEmpty())
		model = settings.value("assistant/model").toString();
	modelCO_->setEditText(model);

	render();
	updateButtons();
}


GuiAssistant::~GuiAssistant()
{
	if (reply_)
		reply_->abort();
}


bool GuiAssistant::initialiseParams(string const &)
{
	return true;
}


void GuiAssistant::restoreSession()
{
}


void GuiAssistant::saveSession(QSettings & settings) const
{
	settings.setValue("assistant/websearch", webTB_->isChecked());
	settings.setValue("assistant/context", contextCO_->currentIndex());
	settings.setValue("assistant/model", modelCO_->currentText());
}


void GuiAssistant::showEvent(QShowEvent * ev)
{
	DockView::showEvent(ev);
	if (modelCO_->count() == 0)
		fetchModels();
	inputTE_->setFocus();
}


void GuiAssistant::updateView()
{
	updateButtons();
}


bool GuiAssistant::eventFilter(QObject * obj, QEvent * ev)
{
	if (obj == inputTE_ && ev->type() == QEvent::KeyPress) {
		QKeyEvent * ke = static_cast<QKeyEvent *>(ev);
		if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)
		    && !(ke->modifiers() & Qt::ShiftModifier)) {
			if (!reply_)
				send();
			return true;
		}
		if (ke->key() == Qt::Key_Escape && reply_) {
			stop();
			return true;
		}
	}
	return DockView::eventFilter(obj, ev);
}


void GuiAssistant::buildActionsMenu()
{
	for (int i = 0; quick_actions[i].label; ++i) {
		QuickAction const & qa = quick_actions[i];
		QString const label = qt_(qa.label);
		QAction * act = actionsMenu_->addAction(label);
		QString const prompt = QString::fromUtf8(qa.prompt);
		bool const rewrite = qa.rewrite;
		connect(act, &QAction::triggered, this, [this, prompt, rewrite, label]() {
			if (reply_)
				return;
			ask(prompt, rewrite, label);
		});
	}
}


QString GuiAssistant::apiBase()
{
	QString url = toqstr(lyxrc.ai_api_url).trimmed();
	while (url.endsWith('/'))
		url.chop(1);
	if (url.isEmpty())
		url = "http://localhost:8080/v1";
	return url;
}


QString GuiAssistant::searxBase()
{
	QString url = toqstr(lyxrc.searxng_url).trimmed();
	while (url.endsWith('/'))
		url.chop(1);
	return url;
}


QNetworkRequest GuiAssistant::apiRequest(QString const & endpoint) const
{
	QNetworkRequest req{QUrl(apiBase() + endpoint)};
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	if (!lyxrc.ai_api_key.empty())
		req.setRawHeader("Authorization",
			"Bearer " + QByteArray::fromStdString(lyxrc.ai_api_key));
	return req;
}


void GuiAssistant::fetchModels()
{
	QNetworkReply * reply = nam_->get(apiRequest("/models"));
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			setStatus(qt_("Cannot reach the AI server at %1 (%2). "
				"Check Tools > Preferences > Language Settings > Writing Assistant.")
				.arg(apiBase(), reply->errorString()));
			return;
		}
		QJsonArray const models = QJsonDocument::fromJson(reply->readAll())
			.object().value("data").toArray();
		QString const current = modelCO_->currentText();
		modelCO_->clear();
		for (QJsonValue const & m : models)
			modelCO_->addItem(m.toObject().value("id").toString());
		if (!current.isEmpty())
			modelCO_->setEditText(current);
		else if (modelCO_->count() > 0)
			modelCO_->setCurrentIndex(0);
		setStatus(QString());
	});
}


BufferView * GuiAssistant::docView()
{
	return const_cast<BufferView *>(bufferview());
}


QString GuiAssistant::contextText(ContextKind kind, QString & description)
{
	description.clear();
	target_kind_ = CONTEXT_NONE;
	target_text_.clear();
	target_par_id_ = -1;
	target_has_insets_ = false;
	BufferView * bv = docView();
	if (!bv || kind == CONTEXT_NONE)
		return QString();
	Cursor const & cur = bv->cursor();

	if (kind == CONTEXT_AUTO)
		kind = cur.selection() ? CONTEXT_SELECTION : CONTEXT_PARAGRAPH;

	QString text;
	switch (kind) {
	case CONTEXT_SELECTION:
		if (!cur.selection())
			return QString();
		text = toqstr(cur.selectionAsString(false));
		target_has_insets_ = text.contains(QChar(0xFFFC));
		description = qt_("selection");
		break;
	case CONTEXT_PARAGRAPH:
		if (!cur.inTexted())
			return QString();
		text = toqstr(cur.paragraph().asString(AS_STR_INSETS));
		target_par_id_ = cur.paragraph().id();
		target_has_insets_ = !cur.paragraph().insetList().empty();
		description = qt_("current paragraph");
		break;
	case CONTEXT_DOCUMENT: {
		QStringList pars;
		for (Paragraph const & par : bv->buffer().paragraphs())
			pars << toqstr(par.asString(AS_STR_INSETS | AS_STR_NEWLINES));
		text = pars.join("\n\n");
		if (text.size() > max_context)
			text = text.left(max_context) + "\n[...]";
		description = qt_("whole document");
		break;
	}
	default:
		return QString();
	}
	text = text.trimmed();
	if (text.isEmpty()) {
		description.clear();
		return QString();
	}
	target_kind_ = kind;
	target_text_ = text;
#if QT_VERSION >= 0x050E00
	int const words = text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).size();
#else
	int const words = text.split(QRegularExpression("\\s+"), QString::SkipEmptyParts).size();
#endif
	description += qt_(" (%1 words)").arg(words);
	return text;
}


void GuiAssistant::send()
{
	QString const text = inputTE_->toPlainText().trimmed();
	if (text.isEmpty() || reply_)
		return;
	inputTE_->clear();
	ask(text, false);
}


void GuiAssistant::ask(QString const & request, bool rewrite,
	QString const & label)
{
	ContextKind kind = ContextKind(contextCO_->currentData().toInt());
	if (rewrite && kind == CONTEXT_NONE)
		kind = CONTEXT_AUTO;
	QString description;
	QString const context = contextText(kind, description);
	if (rewrite && context.isEmpty()) {
		setStatus(qt_("Select some text or place the cursor in a paragraph first."));
		return;
	}

	QString content = request;
	if (!context.isEmpty()) {
		content = QString("%1\n\nText (%2):\n\"\"\"\n%3\n\"\"\"")
			.arg(request, description, context);
	}
	QJsonObject msg;
	msg.insert("role", "user");
	msg.insert("content", content);
	history_.append(msg);

	Message m;
	m.role = "user";
	m.text = label.isEmpty() ? request : label;
	m.context = description;
	messages_.append(m);
	Message a;
	a.role = "assistant";
	messages_.append(a);

	last_was_rewrite_ = rewrite;
	round_ = 0;
	sources_.clear();
	use_tools_ = webTB_->isChecked() && !searxBase().isEmpty();
	if (use_tools_ && tools_unsupported_) {
		// the server cannot call tools: search first, then ask
		use_tools_ = false;
		messages_.last().notes << qt_("Searching the web for \"%1\"").arg(request.left(80));
		render();
		webSearch(request.left(200), [this](QString const & results) {
			QJsonObject note;
			note.insert("role", "system");
			note.insert("content", "Web search results that may help to answer "
				"the next question (cite them as [n] with their URL):\n" + results);
			history_.insert(history_.size() - 1, note);
			startCompletion();
		});
	} else
		startCompletion();
	updateButtons();
}


QString GuiAssistant::systemPrompt() const
{
	QString lang = "English";
	if (BufferView const * bv = bufferview())
		lang = toqstr(bv->buffer().params().language->display());
	QString p = QString(
		"You are a helpful, precise writing assistant built into LyX, a "
		"document processor. You help the user write, edit and research "
		"their document. Today is %1. The document language is %2; answer "
		"in the language of the user's message.\n"
		"When asked to correct, rewrite or translate text, reply with the "
		"new text only, without any preamble, comments or quotation marks, "
		"so that it can be inserted into the document directly. Keep LaTeX "
		"commands and math as they are. Use plain text there; use Markdown "
		"only for other answers.")
		.arg(QDate::currentDate().toString(Qt::ISODate), lang);
	if (use_tools_)
		p += "\nYou can search the web with the web_search tool and read "
		     "pages with the fetch_url tool. Use them for facts you are not "
		     "sure about or that may have changed recently. Cite the sources "
		     "you used as Markdown links.";
	return p;
}


QJsonArray GuiAssistant::tools() const
{
	auto tool = [](QString const & name, QString const & desc,
	               QString const & param, QString const & pdesc) {
		QJsonObject prop;
		prop.insert("type", "string");
		prop.insert("description", pdesc);
		QJsonObject props;
		props.insert(param, prop);
		QJsonObject params;
		params.insert("type", "object");
		params.insert("properties", props);
		params.insert("required", QJsonArray{param});
		QJsonObject fn;
		fn.insert("name", name);
		fn.insert("description", desc);
		fn.insert("parameters", params);
		QJsonObject t;
		t.insert("type", "function");
		t.insert("function", fn);
		return t;
	};
	return QJsonArray{
		tool("web_search", "Search the web. Returns titles, URLs and snippets "
		     "of the best results.", "query", "The search query"),
		tool("fetch_url", "Read the text of a web page.", "url",
		     "The URL of the page")
	};
}


void GuiAssistant::startCompletion()
{
	QString model = modelCO_->currentText().trimmed();
	if (model.isEmpty() && modelCO_->count() > 0)
		model = modelCO_->itemText(0);

	QJsonArray msgs;
	QJsonObject sys;
	sys.insert("role", "system");
	sys.insert("content", systemPrompt());
	msgs.append(sys);
	for (QJsonValue const & v : history_)
		msgs.append(v);

	QJsonObject body;
	if (!model.isEmpty())
		body.insert("model", model);
	body.insert("messages", msgs);
	body.insert("stream", true);
	if (use_tools_ && round_ < max_rounds)
		body.insert("tools", tools());

	answer_.clear();
	reasoning_ = false;
	streamed_ = false;
	sse_buffer_.clear();
	pending_tools_ = QJsonArray();

	QNetworkRequest req = apiRequest("/chat/completions");
	req.setRawHeader("Accept", "text/event-stream, application/json");
	reply_ = nam_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
	connect(reply_, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
	connect(reply_, SIGNAL(finished()), this, SLOT(onFinished()));
	setStatus(qt_("Waiting for the model..."));
	updateButtons();
}


void GuiAssistant::onReadyRead()
{
	if (!reply_)
		return;
	int const status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (status >= 400)
		// error: read everything in onFinished()
		return;
	processChunk(reply_->readAll());
}


void GuiAssistant::processChunk(QByteArray const & chunk)
{
	if (!streamed_ && !chunk.trimmed().startsWith("data:") && sse_buffer_.isEmpty()) {
		// probably not streaming: keep everything for onFinished()
		sse_buffer_ += chunk;
		return;
	}
	if (!streamed_ && !sse_buffer_.trimmed().startsWith("data:")
	    && !sse_buffer_.isEmpty()) {
		// not streaming after all
		sse_buffer_ += chunk;
		return;
	}
	streamed_ = true;
	sse_buffer_ += chunk;
	int nl;
	while ((nl = sse_buffer_.indexOf('\n')) >= 0) {
		QByteArray line = sse_buffer_.left(nl).trimmed();
		sse_buffer_.remove(0, nl + 1);
		if (!line.startsWith("data:"))
			continue;
		line = line.mid(5).trimmed();
		if (line == "[DONE]")
			continue;
		QJsonObject const obj = QJsonDocument::fromJson(line).object();
		QJsonArray const choices = obj.value("choices").toArray();
		if (choices.isEmpty())
			continue;
		QJsonObject const delta = choices.at(0).toObject().value("delta").toObject();
		QString const content = delta.value("content").toString();
		if (!content.isEmpty()) {
			answer_ += content;
			reasoning_ = false;
		} else if (!delta.value("reasoning_content").toString().isEmpty()
		           || !delta.value("reasoning").toString().isEmpty())
			reasoning_ = true;
		// tool calls come in pieces
		QJsonArray const tcs = delta.value("tool_calls").toArray();
		for (QJsonValue const & v : tcs) {
			QJsonObject const tc = v.toObject();
			int const idx = tc.value("index").toInt(pending_tools_.size());
			while (pending_tools_.size() <= idx) {
				QJsonObject fn;
				fn.insert("name", "");
				fn.insert("arguments", "");
				QJsonObject t;
				t.insert("id", "");
				t.insert("type", "function");
				t.insert("function", fn);
				pending_tools_.append(t);
			}
			QJsonObject t = pending_tools_.at(idx).toObject();
			if (tc.contains("id") && !tc.value("id").toString().isEmpty())
				t.insert("id", tc.value("id").toString());
			QJsonObject fn = t.value("function").toObject();
			QJsonObject const dfn = tc.value("function").toObject();
			if (dfn.contains("name"))
				fn.insert("name", fn.value("name").toString() + dfn.value("name").toString());
			if (dfn.contains("arguments"))
				fn.insert("arguments", fn.value("arguments").toString()
				          + dfn.value("arguments").toString());
			t.insert("function", fn);
			pending_tools_.replace(idx, t);
		}
	}
	if (!messages_.isEmpty())
		messages_.last().text = answer_;
	if (!renderTimer_->isActive())
		renderTimer_->start();
}


void GuiAssistant::onFinished()
{
	QNetworkReply * reply = reply_;
	if (!reply)
		return;
	reply->deleteLater();
	reply_ = nullptr;
	int const status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	QByteArray const rest = reply->readAll();

	if (reply->error() == QNetworkReply::OperationCanceledError) {
		finish(qt_("Stopped."));
		return;
	}
	if (reply->error() != QNetworkReply::NoError || status >= 400) {
		QByteArray const data = sse_buffer_ + rest;
		QString msg = QJsonDocument::fromJson(data).object()
			.value("error").toObject().value("message").toString();
		if (msg.isEmpty())
			msg = QString::fromUtf8(data).left(300);
		if (msg.isEmpty())
			msg = reply->errorString();
		if (use_tools_ && status >= 400 && status < 600 && round_ == 0) {
			// Many local servers or models do not support tools.
			// Fall back to searching before asking.
			LYXERR0("Assistant: request with tools failed (" << status
			        << "), retrying without tools: " << fromqstr(msg));
			tools_unsupported_ = true;
			use_tools_ = false;
			QString query;
			for (int i = messages_.size() - 1; i >= 0; --i)
				if (messages_[i].role == "user") {
					query = messages_[i].text;
					break;
				}
			messages_.last().notes << qt_("Searching the web for \"%1\"").arg(query.left(80));
			render();
			webSearch(query.left(200), [this](QString const & results) {
				QJsonObject note;
				note.insert("role", "system");
				note.insert("content", "Web search results that may help to answer "
					"the last question (cite them as [n] with their URL):\n" + results);
				history_.insert(history_.size() - 1, note);
				startCompletion();
			});
			return;
		}
		finish(qt_("The AI server reported an error: %1").arg(msg));
		return;
	}

	if (!streamed_) {
		// a complete JSON answer
		QJsonObject const obj = QJsonDocument::fromJson(sse_buffer_ + rest).object();
		QJsonObject const message = obj.value("choices").toArray().at(0)
			.toObject().value("message").toObject();
		answer_ = message.value("content").toString();
		pending_tools_ = message.value("tool_calls").toArray();
	} else
		// process what is left
		processChunk(rest + '\n');
	if (!messages_.isEmpty())
		messages_.last().text = answer_;

	if (!pending_tools_.isEmpty() && round_ < max_rounds) {
		// the model wants to use tools
		QJsonObject a;
		a.insert("role", "assistant");
		a.insert("content", answer_.isEmpty() ? QJsonValue() : QJsonValue(answer_));
		QJsonArray calls;
		for (int i = 0; i < pending_tools_.size(); ++i) {
			QJsonObject t = pending_tools_.at(i).toObject();
			if (t.value("id").toString().isEmpty())
				t.insert("id", QString("call_%1_%2").arg(round_).arg(i));
			calls.append(t);
		}
		pending_tools_ = calls;
		a.insert("tool_calls", calls);
		history_.append(a);
		++round_;
		runTools();
		return;
	}

	QJsonObject a;
	a.insert("role", "assistant");
	a.insert("content", answer_);
	history_.append(a);
	finish();
}


void GuiAssistant::runTools()
{
	if (pending_tools_.isEmpty()) {
		startCompletion();
		return;
	}
	QJsonObject const call = pending_tools_.at(0).toObject();
	pending_tools_.removeAt(0);
	QString const id = call.value("id").toString();
	QJsonObject const fn = call.value("function").toObject();
	QString const name = fn.value("name").toString();
	QJsonObject const args = QJsonDocument::fromJson(
		fn.value("arguments").toString().toUtf8()).object();

	auto reply = [this, id](QString const & result) {
		QJsonObject t;
		t.insert("role", "tool");
		t.insert("tool_call_id", id);
		t.insert("content", result);
		history_.append(t);
		runTools();
	};

	if (name == "web_search") {
		QString const q = args.value("query").toString();
		messages_.last().notes << qt_("Searching the web for \"%1\"").arg(q);
		render();
		webSearch(q, reply);
	} else if (name == "fetch_url") {
		QString const url = args.value("url").toString();
		messages_.last().notes << qt_("Reading %1").arg(url);
		render();
		fetchUrl(url, reply);
	} else
		reply(QString("Error: unknown tool %1").arg(name));
}


void GuiAssistant::webSearch(QString const & query,
	std::function<void(QString const &)> const & done)
{
	QString const base = searxBase();
	if (base.isEmpty()) {
		done("Error: web search is not configured.");
		return;
	}
	QUrl url(base + "/search");
	QUrlQuery q;
	q.addQueryItem("q", query);
	q.addQueryItem("format", "json");
	url.setQuery(q);
	QNetworkRequest req(url);
	req.setRawHeader("Accept", "application/json");
	setStatus(qt_("Searching the web..."));
	QNetworkReply * r = nam_->get(req);
	reply_ = r;
	connect(r, &QNetworkReply::finished, this, [this, r, done]() {
		r->deleteLater();
		reply_ = nullptr;
		if (r->error() == QNetworkReply::OperationCanceledError) {
			finish(qt_("Stopped."));
			return;
		}
		if (r->error() != QNetworkReply::NoError) {
			done(QString("Error: the search failed (%1).").arg(r->errorString()));
			return;
		}
		QJsonDocument const doc = QJsonDocument::fromJson(r->readAll());
		if (!doc.isObject()) {
			done("Error: the search engine did not return JSON (enable the json "
			     "format in the SearXNG settings).");
			return;
		}
		QJsonArray const results = doc.object().value("results").toArray();
		QString out;
		int n = 0;
		for (QJsonValue const & v : results) {
			if (n >= 6)
				break;
			QJsonObject const o = v.toObject();
			QString const link = o.value("url").toString();
			sources_ << link;
			out += QString("[%1] %2\nURL: %3\n%4\n\n").arg(++n)
				.arg(o.value("title").toString(), link,
				     o.value("content").toString());
		}
		if (out.isEmpty())
			out = "No results.";
		done(out);
	});
}


void GuiAssistant::fetchUrl(QString const & url,
	std::function<void(QString const &)> const & done)
{
	QUrl const u = QUrl::fromUserInput(url);
	if (!u.isValid() || (u.scheme() != "http" && u.scheme() != "https")) {
		done("Error: invalid URL.");
		return;
	}
	QNetworkRequest req(u);
	req.setRawHeader("User-Agent", "Mozilla/5.0 (LyX writing assistant)");
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                 QNetworkRequest::NoLessSafeRedirectPolicy);
	setStatus(qt_("Reading a web page..."));
	QNetworkReply * r = nam_->get(req);
	reply_ = r;
	connect(r, &QNetworkReply::finished, this, [this, r, done]() {
		r->deleteLater();
		reply_ = nullptr;
		if (r->error() == QNetworkReply::OperationCanceledError) {
			finish(qt_("Stopped."));
			return;
		}
		if (r->error() != QNetworkReply::NoError) {
			done(QString("Error: cannot read the page (%1).").arg(r->errorString()));
			return;
		}
		QByteArray const data = r->read(4 * 1024 * 1024);
		QString const type = r->header(QNetworkRequest::ContentTypeHeader).toString();
		QString text;
		if (type.contains("html")) {
			QString html = QString::fromUtf8(data);
			// QTextDocument would render scripts and styles as text
			html.remove(QRegularExpression("<(script|style|noscript)[^>]*>.*?</\\1>",
				QRegularExpression::CaseInsensitiveOption
				| QRegularExpression::DotMatchesEverythingOption));
			QTextDocument doc;
			doc.setHtml(html);
			text = doc.toPlainText();
		} else if (type.startsWith("text/") || type.contains("json") || type.isEmpty())
			text = QString::fromUtf8(data);
		else
			text = QString("The page has content type %1, which cannot be read.").arg(type);
		text.replace(QRegularExpression("\\n\\s*\\n+"), "\n\n");
		if (text.size() > max_page)
			text = text.left(max_page) + "\n[...]";
		done(text.trimmed());
	});
}


void GuiAssistant::finish(QString const & error)
{
	renderTimer_->stop();
	if (!error.isEmpty()) {
		if (!messages_.isEmpty() && messages_.last().role == "assistant"
		    && messages_.last().text.isEmpty())
			messages_.last().notes << error;
		// keep the conversation consistent: drop the unanswered request
		while (!history_.isEmpty()) {
			QString const role = history_.last().toObject().value("role").toString();
			history_.removeLast();
			if (role == "user")
				break;
		}
	}
	setStatus(error.isEmpty() ? QString() : error);
	render();
	updateButtons();
}


void GuiAssistant::stop()
{
	if (reply_)
		reply_->abort();
}


void GuiAssistant::newChat()
{
	stop();
	history_ = QJsonArray();
	messages_.clear();
	sources_.clear();
	setStatus(QString());
	render();
	updateButtons();
	inputTE_->setFocus();
}


QString GuiAssistant::stripThinking(QString const & s, bool & thinking)
{
	QString out = s;
	out.remove(QRegularExpression("<think>.*?</think>",
		QRegularExpression::DotMatchesEverythingOption));
	int const open = out.indexOf("<think>");
	thinking = open >= 0;
	if (thinking)
		out = out.left(open);
	return out.trimmed();
}


QString GuiAssistant::lastReply() const
{
	for (int i = messages_.size() - 1; i >= 0; --i) {
		if (messages_[i].role == "assistant") {
			bool thinking;
			return stripThinking(messages_[i].text, thinking);
		}
	}
	return QString();
}


void GuiAssistant::render()
{
	QString md;
	if (messages_.isEmpty()) {
		md = qt_("### Writing Assistant\n\n"
			"Ask a question about your document, or select some text "
			"and choose one of the **Actions** (proofread, rewrite, "
			"summarize...).\n\n"
			"The text sent along with your message is chosen below "
			"(by default the selection, or else the current paragraph). "
			"Switch on **Web** to let the assistant search the internet.");
	}
	for (int i = 0; i < messages_.size(); ++i) {
		Message const & m = messages_[i];
		if (m.role == "user") {
			QString t = m.text.toHtmlEscaped();
			t.replace("\n", "  \n> ");
			md += "> **" + qt_("You") + "**";
			if (!m.context.isEmpty())
				md += " &nbsp;·&nbsp; *" + qt_("about the %1").arg(m.context) + "*";
			md += "\n>\n> " + t + "\n\n";
		} else {
			for (QString const & n : m.notes)
				md += "*" + n.toHtmlEscaped() + "*\n\n";
			bool thinking = false;
			QString const t = stripThinking(m.text, thinking);
			bool const active = reply_ && i == messages_.size() - 1;
			if (!t.isEmpty())
				md += t + "\n\n";
			else if (active)
				md += "*" + (thinking || reasoning_ ? qt_("Thinking...") : qt_("...")) + "*\n\n";
			if (i < messages_.size() - 1)
				md += "---\n\n";
		}
	}
	QScrollBar * sb = transcript_->verticalScrollBar();
	bool const at_end = sb->value() >= sb->maximum() - 4;
#if QT_VERSION >= 0x050E00
	transcript_->setMarkdown(md);
#else
	transcript_->setPlainText(md);
#endif
	if (at_end || !reply_)
		sb->setValue(sb->maximum());
}


void GuiAssistant::setStatus(QString const & s)
{
	statusLA_->setText(s);
	statusLA_->setVisible(!s.isEmpty());
}


void GuiAssistant::updateButtons()
{
	bool const busy = reply_;
	sendPB_->setText(busy ? qt_("Stop") : qt_("Send"));
	QObject::disconnect(sendPB_, SIGNAL(clicked()), this, nullptr);
	if (busy)
		connect(sendPB_, SIGNAL(clicked()), this, SLOT(stop()));
	else
		connect(sendPB_, SIGNAL(clicked()), this, SLOT(send()));
	actionsTB_->setEnabled(!busy);
	bool const has_reply = !busy && !lastReply().isEmpty();
	bool const has_doc = bufferview() && !bufferview()->buffer().isReadonly();
	replyBar_->setVisible(has_reply);
	insertTB_->setEnabled(has_reply && has_doc);
	replaceTB_->setVisible(last_was_rewrite_);
	replaceTB_->setEnabled(has_reply && has_doc && target_kind_ != CONTEXT_NONE
	                       && target_kind_ != CONTEXT_DOCUMENT);
}


bool GuiAssistant::insertIntoDocument(QString const & text)
{
	BufferView * bv = docView();
	if (!bv || bv->buffer().isReadonly())
		return false;
	if (!bv->cursor().inTexted()) {
		setStatus(qt_("Place the cursor in the text first."));
		return false;
	}
	bv->insertPlaintextString(qstring_to_ucs4(text), true);
	bv->buffer().markDirty();
	bv->buffer().updateBuffer();
	bv->processUpdateFlags(Update::Force | Update::FitCursor);
	const_cast<GuiView &>(lyxview()).updateDialogs();
	return true;
}


void GuiAssistant::insertReply()
{
	QString const text = lastReply();
	if (text.isEmpty())
		return;
	if (insertIntoDocument(text))
		setStatus(qt_("Inserted. Use Undo to revert."));
}


void GuiAssistant::replaceWithReply()
{
	QString const text = lastReply();
	BufferView * bv = docView();
	if (text.isEmpty() || !bv)
		return;
	Cursor & cur = bv->cursor();
	if (target_kind_ == CONTEXT_PARAGRAPH && target_par_id_ >= 0) {
		DocIterator dit = bv->buffer().getParFromID(target_par_id_);
		if (dit.atEnd()) {
			setStatus(qt_("The paragraph does not exist anymore."));
			return;
		}
		dit.pos() = 0;
		bv->putSelectionAt(dit, dit.lastpos(), false);
	}
	if (!cur.selection()
	    || toqstr(cur.selectionAsString(false)).trimmed() != target_text_) {
		setStatus(qt_("The text has changed since the request. Select the text "
			"to replace and use \"Insert\" instead."));
		return;
	}
	if (insertIntoDocument(text))
		setStatus(target_has_insets_
			? qt_("Replaced. Math, citations and other insets of the original "
			      "text were inserted as plain text; use Undo to revert.")
			: qt_("Replaced. Use Undo to revert."));
}


void GuiAssistant::copyReply()
{
	QString const text = lastReply();
	if (!text.isEmpty())
		QApplication::clipboard()->setText(text);
	setStatus(qt_("Copied to the clipboard."));
}


} // namespace frontend
} // namespace lyx

#include "moc_GuiAssistant.cpp"
