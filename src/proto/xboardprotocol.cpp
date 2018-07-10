/***************************************************************************
    File                 : xboardprotocol.cpp
    Project              : Knights
    Description          : Wrapper for the XBoard protocol
    --------------------------------------------------------------------
    Copyright            : (C) 2016 by Alexander Semke (alexander.semke@web.de)
    Copyright            : (C) 2009-2011 by Miha Čančula (miha@noughmad.eu)

 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *  This program is free software; you can redistribute it and/or modify   *
 *  it under the terms of the GNU General Public License as published by   *
 *  the Free Software Foundation; either version 2 of the License, or      *
 *  (at your option) any later version.                                    *
 *                                                                         *
 *  This program is distributed in the hope that it will be useful,        *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *  GNU General Public License for more details.                           *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the Free Software           *
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor,                    *
 *   Boston, MA  02110-1301  USA                                           *
 *                                                                         *
 ***************************************************************************/
#include "proto/xboardprotocol.h"
#include "gamemanager.h"
#include "knightsdebug.h"

#include <KShell>
#include <KProcess>
#include <QFileDialog>
#include <QTimer>

using namespace Knights;

XBoardProtocol::XBoardProtocol(QObject* parent) : ComputerProtocol(parent),
    m_resumePending(false),
    m_moves(0),
    m_increment(0),
    m_baseTime(0),
    m_timeLimit(0),
    m_initComplete(false),
    m_features(NoFeatures),
    m_usermove(false) {
}

Protocol::Features XBoardProtocol::supportedFeatures() {
    // FIXME: This shouldn't be hardcoded. For instance, a chess engine which
    // supports the XBoard protocol may or may not support the pause action; we
    // should find this out using the 'protover' command which will reply with
    // the 'feature' command. See the XBoard protocol specification:
    // http://home.hccnet.nl/h.g.muller/engine-intf.html

    //return GameOver | Pause | Draw | Adjourn | Resign | Undo | SetDifficulty | AdjustDifficulty;
    // return GameOver | Pause | Draw | Resign | Undo | SetDifficulty | AdjustDifficulty;
    return this->m_features;
}

XBoardProtocol::~XBoardProtocol() {
    if ( mProcess && mProcess->isOpen() ) {
        write("quit");
        if ( !mProcess->waitForFinished ( 500 ) )
            mProcess->kill();
    }
}

void XBoardProtocol::startGame() {
    qCDebug(LOG_KNIGHTS) << colorName(color());
    TimeControl c = Manager::self()->timeControl(White);
    if ( c.baseTime != QTime() )
        write(QString(QLatin1String("level %1 %2 %3")).arg(c.moves).arg(QTime().secsTo(c.baseTime)/60).arg(c.increment));

    if (color() == White)
        write("go");

    m_resumePending = false;
}

void XBoardProtocol::executeNextMove() {
    const Move m = this->m_nextUserMove;
    QString str = m.string(false);
    if (m.promotedType())
        str = str.toLower(); // "e7e8q" is used for the pawn promotion -> convert Q in the move string to lowercase q


    if (this->m_usermove) {
        str = QStringLiteral("usermove ") + str;
    }
    qCDebug(LOG_KNIGHTS) << "Player's move:" << str;
    write(str);

    lastMoveString.clear();
    emit undoPossible ( false );
    if ( m_resumePending ) {
        write("go");
        m_resumePending = false;
    }
}

void XBoardProtocol::move ( const Move& m ) {
    this->m_nextUserMove = m;
    QTimer::singleShot(1500, this, &XBoardProtocol::executeNextMove);
}

void XBoardProtocol::init (  ) {
    this->m_features = GameOver | Resign | Undo | SetDifficulty | AdjustDifficulty;
    this->m_initComplete = false;
    this->m_usermove = false;

    startProgram();
    write("xboard");
    write("protover 2");
    // now in parseLine we wait wait for "feature" line
}

QList< Protocol::ToolWidgetData > XBoardProtocol::toolWidgets() {
    return ComputerProtocol::toolWidgets();
}

bool XBoardProtocol::parseStub(const QString& line) {
    parseLine(line);
    return true;
}

bool XBoardProtocol::parseLine(const QString& line) {
    writeToConsole (line, ChatWidget::GeneralMessage);
    if (line.isEmpty()) {
        return true;
    }

    // parse features
    if (!this->m_initComplete) {
        if (line.startsWith(QStringLiteral("feature "))) {
            QStringList features = KShell::splitArgs(line);
            qInfo() << features;
            Q_FOREACH(auto feature, features) {
                if (!feature.contains(QStringLiteral("="))) {
                    continue;
                }

                auto key = feature.split(QStringLiteral("="))[0].trimmed();
                auto value = feature.section(QStringLiteral("="), 1).trimmed();
                qInfo() << key << QStringLiteral("==") << value;

                if (key == QStringLiteral("time") && value == QStringLiteral("1")) {
                    this->m_features |= SetTimeLimit;
                } else if (key == QStringLiteral("pause") && value == QStringLiteral("1")) {
                    this->m_features |= Pause;
                } else if (key == QStringLiteral("draw") && value == QStringLiteral("1")) {
                    this->m_features |= Draw;
                } else if (key == QStringLiteral("usermove") && value == QStringLiteral("1")) {
                    this->m_usermove = true;
                } else if (key == QStringLiteral("done") && value == QStringLiteral("1")) {
                    qInfo() << QStringLiteral("Init complete!");
                    this->m_initComplete = true;
                    initComplete();
                    return true;
                }
            }
        } else if (line.startsWith(QStringLiteral("Illegal move"))) {
            qInfo() << QStringLiteral("Init complete without protover 2!");
            this->m_initComplete = true;
            initComplete();
            return true;
        }
        return true;
    }

    //suppress "Invalid move" replies coming from GNU Chess.
    //TODO: why do we have them?
    if (line.contains(QLatin1String("Invalid move")))
        return true;

    if ( line.contains ( QLatin1String ( "Illegal move" ) ) ) {
        emit illegalMove();
    } else if ( line.contains ( QLatin1String ( " ... " ) ) || line.startsWith(QLatin1String("move")) ) {
        const QRegExp position(QLatin1String("[a-h][1-8]"));
        QString moveString = line.split ( QLatin1Char ( ' ' ) ).last();
        if ( moveString == lastMoveString )
            return true;
        lastMoveString = moveString;
        Move m;
        if ( position.indexIn(line) > -1 )
            m.setString(moveString);
        else if ( moveString.contains(QLatin1String("O-O-O"))
                  || moveString.contains(QLatin1String("o-o-o"))
                  || moveString.contains(QLatin1String("0-0-0")) )
            m = Move::castling(Move::QueenSide, Manager::self()->activePlayer());
        else if ( moveString.contains(QLatin1String("O-O"))
                  || moveString.contains(QLatin1String("o-o"))
                  || moveString.contains(QLatin1String("0-0")) )
            m = Move::castling(Move::KingSide, Manager::self()->activePlayer());

        if ( m.isValid() ) {
            qCDebug(LOG_KNIGHTS) << "Move by" << attribute("program").toString() << ":" << moveString << "=>" << m;
            emit pieceMoved ( m );
            emit undoPossible ( true );
        }
    } else if ( line.contains ( QLatin1String ( "wins" ) ) ) {
        Color winner;
        if ( line.split ( QLatin1Char ( ' ' ) ).last().contains ( QLatin1String ( "white" ) ) )
            winner = White;
        else
            winner = Black;
        emit gameOver ( winner );
        return true;
    } else if ( line.contains ( QLatin1String("offer") ) && line.contains ( QLatin1String("draw") ) ) {
        Offer o;
        o.action = ActionDraw;
        o.id = 0;
        o.player = color();
        Manager::self()->sendOffer(o);
    } else if ( line.startsWith ( QLatin1String("1-0") ) )
        emit gameOver ( White );
    else if ( line.startsWith ( QLatin1String("0-1") ) )
        emit gameOver ( Black );
    else if ( line.startsWith ( QLatin1String("1/2-1/2") ) )
        emit gameOver ( NoColor );
    return true;
}

void XBoardProtocol::acceptOffer(const Offer& offer) {
    qCDebug(LOG_KNIGHTS) << "Accepting offer" << offer.text;
    switch ( offer.action ) {
        case ActionDraw:
            setWinner(NoColor);
            break;

        case ActionAdjourn:
            write( QLatin1String("save ") + QFileDialog::getSaveFileName() );
            break;

        case ActionUndo:
            for ( int i = 0; i < offer.numberOfMoves/2; ++i )
                write ( "remove" );
            if (offer.numberOfMoves % 2) {
                write ( "force" );
                write ( "undo" );

                if ( Manager::self()->activePlayer() != color() )
                    write ( "go" );
                else
                    m_resumePending = true;
            }
            break;

        case ActionPause:
            write ( "force" );
            break;

        case ActionResume:
            if ( Manager::self()->activePlayer() == color() )
                write ( "go" );
            else
                m_resumePending = true;
            break;

        default:
            qCCritical(LOG_KNIGHTS) << "XBoard should not send this kind offers";
            break;
    }
}

void XBoardProtocol::declineOffer(const Offer& offer) {
    // No special action to do here, ignoring an offer is the same as declining.
    Q_UNUSED(offer);
}

void XBoardProtocol::setWinner(Color winner) {
    QByteArray result = "result ";
    switch ( winner ) {
        case White:
            result += "1-0";
            break;
        case Black:
            result += "0-1";
            break;
        case NoColor:
            result += "1/2-1/2";
            break;
    }
    write(QLatin1String(result));
}

void XBoardProtocol::makeOffer(const Offer& offer) {
    switch ( offer.action ) {
        case ActionDraw:
            write("draw");
            break;

        case ActionAdjourn:
            write( QLatin1String("save ") + QFileDialog::getSaveFileName() );
            offer.accept();
            break;

        case ActionUndo:
            for ( int i = 0; i < offer.numberOfMoves/2; ++i )
                write ( "remove" );
            if (offer.numberOfMoves % 2) {
                write ( "force" );
                write ( "undo" );

                if ( Manager::self()->activePlayer() != color() )
                    write ( "go" );
                else
                    m_resumePending = true;
            }
            offer.accept();
            break;

        case ActionPause:
            write ( "force" );
            offer.accept();
            break;

        case ActionResume:
            if ( Manager::self()->activePlayer() == color() )
                write ( "go" );
            else
                m_resumePending = true;
            offer.accept();
            break;

        default:
            break;
    }
}

void XBoardProtocol::setDifficulty(int depth, int memory) {
    // Gnuchess only supports 'depth', while Crafty (and the XBoard protocol) wants 'sd'.
    // So we give both.
    /* write ( QLatin1String("depth ") + QString::number ( depth ) ); */
    write ( QLatin1String("easy") );
    write ( QLatin1String("sd ") + QString::number ( depth ) );
    /* write ( QLatin1String("memory ") + QString::number ( memory ) ); */
}
