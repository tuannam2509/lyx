// -*- C++ -*-
/**
 * \file GuiAssistant.h
 * This file is part of LyX, the document processor.
 * Licence details can be found in the file COPYING.
 *
 * A side panel to chat with an AI model (OpenAI-compatible API) about
 * the document, with optional web search through SearXNG.
 *
 * Full author contact details are available in file CREDITS.
 */

#ifndef GUIASSISTANT_H
#define GUIASSISTANT_H

#include "DockView.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QComboBox;
class QLabel;
class QMenu;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
class QTimer;
class QToolButton;
class QWidget;

namespace lyx {
namespace frontend {

class GuiAssistant : public DockView
{
	Q_OBJECT

public:
	GuiAssistant(
		GuiView & parent, ///< the main window where to dock.
		Qt::DockWidgetArea area = Qt::RightDockWidgetArea,
		Qt::WindowFlags flags = {});
	~GuiAssistant();

	/// \name Dialog inherited methods
	//@{
	bool initialiseParams(std::string const &) override;
	void clearParams() override {}
	void dispatchParams() override {}
	bool isBufferDependent() const override { return false; }
	bool canApply() const override { return true; }
	bool canApplyToReadOnly() const override { return true; }
	void updateView() override;
	bool wantInitialFocus() const override { return true; }
	void restoreSession() override;
	void saveSession(QSettings & settings) const override;
	//@}

protected:
	bool eventFilter(QObject * obj, QEvent * ev) override;
	void showEvent(QShowEvent *) override;

private Q_SLOTS:
	void send();
	void stop();
	void newChat();
	void insertReply();
	void replaceWithReply();
	void copyReply();
	void fetchModels();
	void onReadyRead();
	void onFinished();
	void render();

private:
	/// what part of the document is sent along with the request
	enum ContextKind {
		CONTEXT_AUTO,       ///< selection, else current paragraph
		CONTEXT_SELECTION,
		CONTEXT_PARAGRAPH,
		CONTEXT_DOCUMENT,
		CONTEXT_NONE
	};

	/// a message as shown in the transcript
	struct Message {
		QString role;     ///< "user", "assistant" or "note"
		QString text;     ///< as typed / as received
		QString context;  ///< description of the attached context
		QStringList notes; ///< tool activity (searches...)
	};

	/// send a request with the given text; \p quick is a quick action
	void ask(QString const & request, bool rewrite,
	         QString const & label = QString());
	/// collect the context text from the document
	QString contextText(ContextKind kind, QString & description);
	/// parse received (streamed) data
	void processChunk(QByteArray const & chunk);
	/// start a (streaming) completion request
	void startCompletion();
	/// run the requested tools, then continue the conversation
	void runTools();
	/// search the web with SearXNG; calls done with the result text
	void webSearch(QString const & query,
	               std::function<void(QString const &)> const & done);
	/// fetch a web page as plain text
	void fetchUrl(QString const & url,
	              std::function<void(QString const &)> const & done);
	/// the conversation finished (successfully or not)
	void finish(QString const & error = QString());
	/// the system prompt
	QString systemPrompt() const;
	/// tool definitions (OpenAI function calling)
	QJsonArray tools() const;
	/// the text of the last answer, without "thinking" parts
	QString lastReply() const;
	/// remove <think> blocks
	static QString stripThinking(QString const & s, bool & thinking);
	/// base URLs
	static QString apiBase();
	static QString searxBase();
	/// network request with authorization
	QNetworkRequest apiRequest(QString const & endpoint) const;
	/// enable/disable widgets according to the state
	void updateButtons();
	/// show a message below the transcript
	void setStatus(QString const & s);
	/// fill the quick actions menu
	void buildActionsMenu();
	/// mutable access to the current document view
	BufferView * docView();
	/// insert text at the cursor, replacing the selection
	bool insertIntoDocument(QString const & text);

	QWidget * panel_;
	QComboBox * modelCO_;
	QToolButton * newChatTB_;
	QTextBrowser * transcript_;
	QComboBox * contextCO_;
	QToolButton * webTB_;
	QToolButton * actionsTB_;
	QMenu * actionsMenu_;
	QPlainTextEdit * inputTE_;
	QPushButton * sendPB_;
	QWidget * replyBar_;
	QToolButton * insertTB_;
	QToolButton * replaceTB_;
	QToolButton * copyTB_;
	QLabel * statusLA_;
	QTimer * renderTimer_;

	QNetworkAccessManager * nam_;
	QPointer<QNetworkReply> reply_;
	/// SSE data not yet processed
	QByteArray sse_buffer_;
	/// did the server stream (server sent events)?
	bool streamed_;

	/// the conversation as sent to the API (without system prompt)
	QJsonArray history_;
	/// the conversation as displayed
	QVector<Message> messages_;
	/// tool calls requested by the model in the current round
	QJsonArray pending_tools_;
	/// current answer: content and reasoning
	QString answer_;
	bool reasoning_;
	/// round of tool calls in the current turn
	int round_;
	/// use tools (function calling) in this request
	bool use_tools_;
	/// the server rejected tools: search before asking instead
	bool tools_unsupported_;
	/// sources found by web searches in this turn
	QStringList sources_;

	/// what the last request referred to, for "Replace"
	ContextKind target_kind_;
	QString target_text_;
	int target_par_id_;
	/// the target paragraph contains insets (math, citations...)
	bool target_has_insets_ = false;
	bool last_was_rewrite_;
};

} // namespace frontend
} // namespace lyx

#endif // GUIASSISTANT_H
