
#include "qinterprocesschannel.h"

/*!
	\file qinterprocesschannel.cpp
	
	\brief Implementation of the QInterProcessChannel class.
*/

#include "qmanagedsocket.h"
#include "qmanagedrequest.h"

#include <QDir>
#include <QFile>
#include <QTimer>
#include <QSettings>
#include <QTcpSocket>
#include <QTcpServer>
#include <QApplication>

/*!
	\class QInterProcessChannel
	
	\brief A generic network-based communication channel for applications.
	
	QInterProcessChannel offers communication facilities between several instances of
	the same application. It initializes itself so that the first created instance acts
	as a "server" which recieve messages from the others instances. This is particularly
	handy when creating a single-instance application which, for example, uses file
	association through a command line interface.
	
	\see QSingleApplication
*/

/*!
	\brief Constructor
	
	Check for a valid server and create one if none found.
*/
QInterProcessChannel::QInterProcessChannel(QObject *p)
 : QThread(p), pServer(0), pServerTimer(0)
{
	init();
}

/*!
	\brief Destructor
*/
QInterProcessChannel::~QInterProcessChannel()
{
	while ( isRunning() )
		quit();
	
	close();
}

/*!
	\return Wether the instance has a server role
*/
bool QInterProcessChannel::isServer() const
{
	return pServer;
}

/*!
	\return The current buffered message
*/
QString QInterProcessChannel::messageBuffer() const
{
	return sMsg;
}

/*!
	\brief Sets a buffered message
*/
void QInterProcessChannel::setMessageBuffer(const QString& s)
{
	sMsg = s;
}

/*!
	\overload
	
	Sends the content of the current message buffer
*/
void QInterProcessChannel::sendMessage()
{
	sendMessage(sMsg);
	sMsg.clear();
}

/*!
	\overload
*/
void QInterProcessChannel::sendMessage(const QString& s)
{
	sendMessage(s.toLocal8Bit());
}
/*!
	\brief Send a message to server instance
*/
void QInterProcessChannel::sendMessage(const QByteArray& msg)
{
	if ( !pServer && msg.count() )
	{
		//qDebug("Sending msg : %s", msg.constData());
		
		QTcpSocket *pSocket = new QTcpSocket(this);
		pSocket->connectToHost(m_addr, m_port);
		pSocket->waitForConnected();
		pSocket->write(msg);
		pSocket->waitForBytesWritten();
		
	} else {
		//qWarning("Empty messages are not carried out...");
	}
}

/*!
	\brief Closes the communication channel
	
	\see reconnect()
*/
void QInterProcessChannel::close()
{
	if ( pServer )
	{
		pServer->close();
		delete pServer;
		pServer = 0;
		
		QString ini = QDir::tempPath() + QDir::separator() + QApplication::applicationName() + "rc";
		QFile::remove(ini);
	}
}

/*!
	\brief Attemps to reconnect
	
	When the server instance is closed (or crashes...) the connectionLost() signal
	is emitted. As an alternative to closing the client instances it is possible to
	try a reconnection so that one of theclient will become a server.
	
	\note When several clients are running side by side this function is very likely
	to cause a fork and create several (independant) clients whose only one will be
	reachable by newer clients...
	
	\see connectionLost()
*/
void QInterProcessChannel::reconnect()
{
	//qDebug("attempting to reconnect");
	init();
}

/*!
	\internal
*/
void QInterProcessChannel::run()
{
	/*
		There we check that the server lives...
		If connection fails notify it...
	*/
	
	if ( pServer )
		return;
	
	//qDebug("timer setup...");
	
	pServerTimer = new QTimer;
	pServerTimer->setInterval(100);
	pServerTimer->setSingleShot(false);
	
	connect(pServerTimer, SIGNAL( timeout() ),
			this		, SLOT  ( check() ) );
	
	pServerTimer->start();
	
	exec();
}

/*!
	\internal
*/
void QInterProcessChannel::check()
{
	//qDebug("checking connection...");
	
	QTcpSocket *pSocket = new QTcpSocket(this);
	pSocket->connectToHost(m_addr, m_port);
	
	if ( pSocket->error() != -1 )
	{
		emit connectionLost();
		return;
	}
	
	pSocket->waitForConnected();
	
	if ( pSocket->error() != -1 )
	{
		emit connectionLost();
		return;
	}
}

/*!
	\internal
*/
void QInterProcessChannel::init()
{
	while ( isRunning() )
		quit();
	
	if ( pServerTimer )
	{
		pServerTimer->stop();
		delete pServerTimer;
		pServerTimer = 0;
	}
	
	bool ok = true;
	
	m_port = 0;
	m_addr = QHostAddress::LocalHost;
	QString ini = QDir::tempPath() + QDir::separator() + QApplication::applicationName() + "rc";
	
	pServer = new QTcpServer(this);
	pServer->listen(m_addr, m_port);
	
	connect(pServer	, SIGNAL( newConnection() ),
			this	, SLOT  ( connection() ) );
	
	if ( QFile::exists(ini) )
	{
		/*
			found a server config file, let us assume it is from a running server
		*/
		//qDebug("checking old server...");
		
		QSettings conf(ini, QSettings::IniFormat);
		
		m_port = conf.value("port").toUInt();
		m_addr = QHostAddress(conf.value("address").toString());
		
		QTcpSocket *pSocket = new QTcpSocket(this);
		
		if ( !m_addr.isNull() && m_port )
		{
			pSocket->connectToHost(m_addr, m_port);
			ok = pSocket->waitForConnected();
			
			if ( ok ) ok &= pSocket->write("--check");
			if ( ok ) ok &= pSocket->waitForBytesWritten();
			if ( ok ) ok &= pSocket->waitForReadyRead();
			if ( ok ) ok &= (pSocket->readAll() == "[ALIVE]");
			
		} else {
			ok = false;
		}
		
		if ( !ok )
		{
			QFile::remove(ini);
		}
		
		pSocket->disconnectFromHost();
		delete pSocket;
	}
	
	if ( !QFile::exists(ini) )
	{
		// no server found... Create one
		//qDebug("setting up new server...");
		
		m_port = pServer->serverPort();
		m_addr = pServer->serverAddress();
		
		QSettings conf(ini, QSettings::IniFormat);
		conf.setValue("port", m_port);
		conf.setValue("address", m_addr.toString());
		
		emit gotServerRole();
		emit serverRoleChanged(true);
	} else {
		// server found we'll hook on it later on...
		pServer->close();
		pServer = 0;
		
		emit serverRoleChanged(false);
	}
	
	start();
}

/*!
	\internal
*/
void QInterProcessChannel::message(const QString& msg, QManagedSocket *s)
{
	if ( !pServer )
		return;
	
	//qDebug("message from Inter Process channel : %s", qPrintable(msg));
	
	QStringList argv = QManagedRequest::splitArguments(msg);
	const QString cmd = argv.at(0);
	//const int argc = argv.count();
	
	if ( cmd == "--check" )
	{
		s->send("[ALIVE]");
	} else if ( cmd == "--request" ) {
		argv.removeAt(0);
		emit request(argv);
	} else {
		emit message(msg);
	}
}

/*!
	\brief internal
*/
void QInterProcessChannel::connection()
{
	if ( !pServer )
		return;
	
	//qDebug("incoming Inter Process connection");
	
	while ( pServer->hasPendingConnections() )
	{
		QManagedSocket *s = new QManagedSocket(pServer->nextPendingConnection(), this);
		
		connect(s	, SIGNAL( message(QString, QManagedSocket*) ),
				this, SLOT  ( message(QString, QManagedSocket*) ) );
	}
}
