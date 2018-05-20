/**************************************************************************
* Otter Browser: Web browser controlled by the user, not vice-versa.
* Copyright (C) 2018 Michal Dutkiewicz aka Emdek <michal@emdek.pl>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*
**************************************************************************/

#include "FeedsModel.h"
#include "Console.h"
#include "FeedsManager.h"
#include "SessionsManager.h"
#include "ThemesManager.h"
#include "Utils.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>
#include <QtCore/QXmlStreamReader>
#include <QtCore/QXmlStreamWriter>
#include <QtWidgets/QMessageBox>

namespace Otter
{

FeedsModel::Entry::Entry(Feed *feed) : QStandardItem(),
	m_feed(feed)
{
}

QVariant FeedsModel::Entry::data(int role) const
{
	if (role == Qt::DisplayRole)
	{
		const EntryType type(static_cast<EntryType>(data(TypeRole).toInt()));

		switch (type)
		{
			case RootEntry:
				return QCoreApplication::translate("Otter::FeedsModel", "Feeds");
			case TrashEntry:
				return QCoreApplication::translate("Otter::FeedsModel", "Trash");
			case FolderEntry:
				if (QStandardItem::data(role).isNull())
				{
					return QCoreApplication::translate("Otter::FeedsModel", "(Untitled)");
				}

				break;
			case FeedEntry:
				if (m_feed && !m_feed->getTitle().isEmpty())
				{
					return m_feed->getTitle();
				}

				break;
			default:
				break;
		}
	}

	if (role == Qt::DecorationRole)
	{
		switch (static_cast<EntryType>(data(TypeRole).toInt()))
		{
			case RootEntry:
			case FolderEntry:
				return ThemesManager::createIcon(QLatin1String("inode-directory"));
			case TrashEntry:
				return ThemesManager::createIcon(QLatin1String("user-trash"));
			case FeedEntry:
				if (m_feed && !m_feed->getIcon().isNull())
				{
					return m_feed->getIcon();
				}

				return ThemesManager::createIcon(QLatin1String("application-rss+xml"));
			default:
				break;
		}

		return {};
	}

	if (m_feed)
	{
		switch (role)
		{
			case LastUpdateTimeRole:
				return m_feed->getLastUpdateTime();
			case LastSynchronizationTimeRole:
				return m_feed->getLastSynchronizationTime();
			case UrlRole:
				return m_feed->getUrl();
			default:
				break;
		}
	}

	if (role == IsTrashedRole)
	{
		QModelIndex parent(index().parent());

		while (parent.isValid())
		{
			const EntryType type(static_cast<EntryType>(parent.data(TypeRole).toInt()));

			if (type == RootEntry)
			{
				break;
			}

			if (type == TrashEntry)
			{
				return true;
			}

			parent = parent.parent();
		}

		return false;
	}

	return QStandardItem::data(role);
}

QVariant FeedsModel::Entry::getRawData(int role) const
{
	return QStandardItem::data(role);
}

bool FeedsModel::Entry::isAncestorOf(FeedsModel::Entry *child) const
{
	if (child == nullptr || child == this)
	{
		return false;
	}

	QStandardItem *parent(child->parent());

	while (parent)
	{
		if (parent == this)
		{
			return true;
		}

		parent = parent->parent();
	}

	return false;
}

FeedsModel::FeedsModel(const QString &path, QObject *parent) : QStandardItemModel(parent),
	m_rootEntry(new Entry()),
	m_trashEntry(new Entry())
{
	m_rootEntry->setData(RootEntry, TypeRole);
	m_rootEntry->setDragEnabled(false);
	m_trashEntry->setData(TrashEntry, TypeRole);
	m_trashEntry->setDragEnabled(false);
	m_trashEntry->setEnabled(false);

	appendRow(m_rootEntry);
	appendRow(m_trashEntry);
	setItemPrototype(new Entry());

	if (!QFile::exists(path))
	{
		return;
	}

	QFile file(path);

	if (!file.open(QIODevice::ReadOnly))
	{
		Console::addMessage(tr("Failed to open feeds file: %1").arg(file.errorString()), Console::OtherCategory, Console::ErrorLevel, path);

		return;
	}

	QXmlStreamReader reader(&file);

	if (reader.readNextStartElement() && reader.name() == QLatin1String("opml") && reader.attributes().value(QLatin1String("version")).toString() == QLatin1String("1.0"))
	{
		while (reader.readNextStartElement())
		{
			if (reader.name() == QLatin1String("outline"))
			{
				readEntry(&reader, m_rootEntry);
			}

			if (reader.name() != QLatin1String("body"))
			{
				reader.skipCurrentElement();
			}

			if (reader.hasError())
			{
				Console::addMessage(tr("Failed to load feeds file: %1").arg(reader.errorString()), Console::OtherCategory, Console::ErrorLevel, file.fileName());

				QMessageBox::warning(nullptr, tr("Error"), tr("Failed to load feeds file."), QMessageBox::Close);

				return;
			}
		}
	}

	file.close();
}

void FeedsModel::trashEntry(Entry *entry)
{
	if (!entry)
	{
		return;
	}

	const EntryType type(static_cast<EntryType>(entry->data(TypeRole).toInt()));

	if (type != RootEntry && type != TrashEntry)
	{
		if (entry->data(IsTrashedRole).toBool())
		{
///TODO
		}
		else
		{
			Entry *previousParent(static_cast<Entry*>(entry->parent()));

			m_trash[entry] = {entry->parent()->index(), entry->row()};

			m_trashEntry->appendRow(entry->parent()->takeRow(entry->row()));
			m_trashEntry->setEnabled(true);

			removeEntryUrl(entry);

			emit entryModified(entry);
			emit entryTrashed(entry, previousParent);
			emit modelModified();
		}
	}
}

void FeedsModel::restoreEntry(Entry *entry)
{
	if (!entry)
	{
		return;
	}

	Entry *formerParent(m_trash.contains(entry) ? getEntry(m_trash[entry].first) : m_rootEntry);

	if (!formerParent || static_cast<EntryType>(formerParent->data(TypeRole).toInt()) != FolderEntry)
	{
		formerParent = m_rootEntry;
	}

	if (m_trash.contains(entry))
	{
		formerParent->insertRow(m_trash[entry].second, entry->parent()->takeRow(entry->row()));

		m_trash.remove(entry);
	}
	else
	{
		formerParent->appendRow(entry->parent()->takeRow(entry->row()));
	}

	readdEntryUrl(entry);

	m_trashEntry->setEnabled(m_trashEntry->rowCount() > 0);

	emit entryModified(entry);
	emit entryRestored(entry);
	emit modelModified();
}

void FeedsModel::removeEntry(Entry *entry)
{
	if (!entry)
	{
		return;
	}

	removeEntryUrl(entry);

	const quint64 identifier(entry->data(IdentifierRole).toULongLong());

	if (identifier > 0 && m_identifiers.contains(identifier))
	{
		m_identifiers.remove(identifier);
	}

	emit entryRemoved(entry, static_cast<Entry*>(entry->parent()));

	entry->parent()->removeRow(entry->row());

	emit modelModified();
}

void FeedsModel::readEntry(QXmlStreamReader *reader, Entry *parent)
{
	const QString title(reader->attributes().value(reader->attributes().hasAttribute(QLatin1String("title")) ? QLatin1String("title") : QLatin1String("text")).toString());

	if (reader->attributes().hasAttribute(QLatin1String("xmlUrl")))
	{
		const QUrl url(Utils::normalizeUrl(QUrl(reader->attributes().value(QLatin1String("xmlUrl")).toString())));

		if (url.isValid())
		{
			Entry *entry(new Entry(FeedsManager::createFeed(title, url, Utils::loadPixmapFromDataUri(reader->attributes().value(QLatin1String("icon")).toString()), reader->attributes().value(QLatin1String("updateInterval")).toInt())));
			entry->setData(FeedEntry, TypeRole);
			entry->setFlags(entry->flags() | Qt::ItemNeverHasChildren);

			parent->appendRow(entry);
		}
	}
	else
	{
		Entry *entry(new Entry());
		entry->setData(FolderEntry, TypeRole);
		entry->setData(title, TitleRole);

		parent->appendRow(entry);

		while (reader->readNext())
		{
			if (reader->isStartElement())
			{
				if (reader->name() == QLatin1String("outline"))
				{
					readEntry(reader, entry);
				}
				else
				{
					reader->skipCurrentElement();
				}
			}
			else if (reader->hasError())
			{
				return;
			}
		}
	}
}

void FeedsModel::writeEntry(QXmlStreamWriter *writer, Entry *entry) const
{
	if (!entry)
	{
		return;
	}

	switch (static_cast<EntryType>(entry->data(TypeRole).toInt()))
	{
		case FolderEntry:
		case FeedEntry:
///TODO

			break;
		default:
			break;
	}
}

void FeedsModel::removeEntryUrl(Entry *entry)
{
	if (!entry)
	{
		return;
	}

	const EntryType type(static_cast<EntryType>(entry->data(TypeRole).toInt()));

	if (type == FeedEntry)
	{
		const QUrl url(Utils::normalizeUrl(entry->data(UrlRole).toUrl()));

		if (!url.isEmpty() && m_urls.contains(url))
		{
			m_urls[url].removeAll(entry);

			if (m_urls[url].isEmpty())
			{
				m_urls.remove(url);
			}
		}
	}
	else if (type == FolderEntry)
	{
		for (int i = 0; i < entry->rowCount(); ++i)
		{
			removeEntryUrl(static_cast<Entry*>(entry->child(i, 0)));
		}
	}
}

void FeedsModel::readdEntryUrl(Entry *entry)
{
	if (!entry)
	{
		return;
	}

	const EntryType type(static_cast<EntryType>(entry->data(TypeRole).toInt()));

	if (type == FeedEntry)
	{
		const QUrl url(Utils::normalizeUrl(entry->data(UrlRole).toUrl()));

		if (!url.isEmpty())
		{
			if (!m_urls.contains(url))
			{
				m_urls[url] = QVector<Entry*>();
			}

			m_urls[url].append(entry);
		}
	}
	else if (type == FolderEntry)
	{
		for (int i = 0; i < entry->rowCount(); ++i)
		{
			readdEntryUrl(static_cast<Entry*>(entry->child(i, 0)));
		}
	}
}

void FeedsModel::emptyTrash()
{
	m_trashEntry->removeRows(0, m_trashEntry->rowCount());
	m_trashEntry->setEnabled(false);

	m_trash.clear();

	emit modelModified();
}

void FeedsModel::handleUrlChanged(Entry *entry, const QUrl &newUrl, const QUrl &oldUrl)
{
	if (!oldUrl.isEmpty() && m_urls.contains(oldUrl))
	{
		m_urls[oldUrl].removeAll(entry);

		if (m_urls[oldUrl].isEmpty())
		{
			m_urls.remove(oldUrl);
		}
	}

	if (!newUrl.isEmpty())
	{
		if (!m_urls.contains(newUrl))
		{
			m_urls[newUrl] = QVector<Entry*>();
		}

		m_urls[newUrl].append(entry);
	}
}

FeedsModel::Entry* FeedsModel::addEntry(EntryType type, const QMap<int, QVariant> &metaData, Entry *parent, int index)
{
	Entry *entry(new Entry());

	if (!parent)
	{
		parent = m_rootEntry;
	}

	parent->insertRow(((index < 0) ? parent->rowCount() : index), entry);

	if (type == FeedEntry)
	{
		entry->setDropEnabled(false);
	}

	if (type == FolderEntry || type == FeedEntry)
	{
		quint64 identifier(metaData.value(IdentifierRole).toULongLong());

		if (identifier == 0 || m_identifiers.contains(identifier))
		{
			identifier = (m_identifiers.isEmpty() ? 1 : (m_identifiers.keys().last() + 1));
		}

		m_identifiers[identifier] = entry;

		QMap<int, QVariant>::const_iterator iterator;

		for (iterator = metaData.begin(); iterator != metaData.end(); ++iterator)
		{
			setData(entry->index(), iterator.value(), iterator.key());
		}

		entry->setData(identifier, IdentifierRole);

		if (type == FeedEntry)
		{
			const QUrl url(metaData.value(UrlRole).toUrl());

			if (!url.isEmpty())
			{
				handleUrlChanged(entry, url);
			}

			entry->setFlags(entry->flags() | Qt::ItemNeverHasChildren);
		}
	}

	entry->setData(type, TypeRole);

	emit entryAdded(entry);
	emit modelModified();

	return entry;
}

FeedsModel::Entry* FeedsModel::getEntry(const QModelIndex &index) const
{
	Entry *entry(static_cast<Entry*>(itemFromIndex(index)));

	if (entry)
	{
		return entry;
	}

	return getEntry(index.data(IdentifierRole).toULongLong());
}

FeedsModel::Entry* FeedsModel::getEntry(quint64 identifier) const
{
	if (identifier == 0)
	{
		return m_rootEntry;
	}

	if (m_identifiers.contains(identifier))
	{
		return m_identifiers[identifier];
	}

	return nullptr;
}

FeedsModel::Entry* FeedsModel::getRootEntry() const
{
	return m_rootEntry;
}

FeedsModel::Entry* FeedsModel::getTrashEntry() const
{
	return m_trashEntry;
}

QMimeData* FeedsModel::mimeData(const QModelIndexList &indexes) const
{
	QMimeData *mimeData(new QMimeData());
	QStringList texts;
	texts.reserve(indexes.count());

	QList<QUrl> urls;
	urls.reserve(indexes.count());

	if (indexes.count() == 1)
	{
		mimeData->setProperty("x-item-index", indexes.at(0));
	}

	for (int i = 0; i < indexes.count(); ++i)
	{
		if (indexes.at(i).isValid() && static_cast<EntryType>(indexes.at(i).data(TypeRole).toInt()) == FeedEntry)
		{
			texts.append(indexes.at(i).data(UrlRole).toString());
			urls.append(indexes.at(i).data(UrlRole).toUrl());
		}
	}

	mimeData->setText(texts.join(QLatin1String(", ")));
	mimeData->setUrls(urls);

	return mimeData;
}

QStringList FeedsModel::mimeTypes() const
{
	return {QLatin1String("text/uri-list")};
}

QVector<FeedsModel::Entry*> FeedsModel::getEntries(const QUrl &url) const
{
	const QUrl normalizedUrl(Utils::normalizeUrl(url));
	QVector<FeedsModel::Entry*> entrys;

	if (m_urls.contains(url))
	{
		entrys = m_urls[url];
	}

	if (url != normalizedUrl && m_urls.contains(normalizedUrl))
	{
		entrys.append(m_urls[normalizedUrl]);
	}

	return entrys;
}

bool FeedsModel::moveFeed(Entry *entry, Entry *newParent, int newRow)
{
	if (!entry || !newParent || entry == newParent || entry->isAncestorOf(newParent))
	{
		return false;
	}

	Entry *previousParent(static_cast<Entry*>(entry->parent()));

	if (!previousParent)
	{
		if (newRow < 0)
		{
			newParent->appendRow(entry);
		}
		else
		{
			newParent->insertRow(newRow, entry);
		}

		emit modelModified();

		return true;
	}

	const int previousRow(entry->row());

	if (newRow < 0)
	{
		newParent->appendRow(entry->parent()->takeRow(entry->row()));

		emit entryMoved(entry, previousParent, previousRow);
		emit modelModified();

		return true;
	}

	int targetRow(newRow);

	if (entry->parent() == newParent && entry->row() < newRow)
	{
		--targetRow;
	}

	newParent->insertRow(targetRow, entry->parent()->takeRow(entry->row()));

	emit entryMoved(entry, previousParent, previousRow);
	emit modelModified();

	return true;
}

bool FeedsModel::canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) const
{
	const QModelIndex index(data->property("x-item-index").toModelIndex());

	if (index.isValid())
	{
		const Entry *entry(getEntry(index));
		Entry *newParent(getEntry(parent));

		return (entry && newParent && entry != newParent && !entry->isAncestorOf(newParent));
	}

	return QStandardItemModel::canDropMimeData(data, action, row, column, parent);
}

bool FeedsModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent)
{
	const EntryType type(static_cast<EntryType>(parent.data(TypeRole).toInt()));

	if (type == FolderEntry || type == RootEntry || type == TrashEntry)
	{
		const QModelIndex index(data->property("x-item-index").toModelIndex());

		if (index.isValid())
		{
			return moveFeed(getEntry(index), getEntry(parent), row);
		}

		if (data->hasUrls())
		{
			const QVector<QUrl> urls(Utils::extractUrls(data));

			for (int i = 0; i < urls.count(); ++i)
			{
				addEntry(FeedEntry, {{UrlRole, urls.at(i)}, {TitleRole, (data->property("x-url-title").toString().isEmpty() ? urls.at(i).toString() : data->property("x-url-title").toString())}}, getEntry(parent), row);
			}

			return true;
		}

		return QStandardItemModel::dropMimeData(data, action, row, column, parent);
	}

	return false;
}

bool FeedsModel::save(const QString &path) const
{
	if (SessionsManager::isReadOnly())
	{
		return false;
	}

	QSaveFile file(path);

	if (!file.open(QIODevice::WriteOnly))
	{
		return false;
	}

	QXmlStreamWriter writer(&file);
	writer.setAutoFormatting(true);
	writer.setAutoFormattingIndent(-1);
	writer.writeStartDocument();
	writer.writeDTD(QLatin1String("<!DOCTYPE xbel>"));
	writer.writeStartElement(QLatin1String("xbel"));
	writer.writeAttribute(QLatin1String("version"), QLatin1String("1.0"));

	for (int i = 0; i < m_rootEntry->rowCount(); ++i)
	{
		writeEntry(&writer, static_cast<Entry*>(m_rootEntry->child(i, 0)));
	}

	writer.writeEndDocument();

	return file.commit();
}

bool FeedsModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
	Entry *entry(getEntry(index));

	if (!entry)
	{
		return QStandardItemModel::setData(index, value, role);
	}

	if (role == UrlRole && value.toUrl() != index.data(UrlRole).toUrl())
	{
		handleUrlChanged(entry, Utils::normalizeUrl(value.toUrl()), Utils::normalizeUrl(index.data(UrlRole).toUrl()));
	}

	entry->setData(value, role);

	switch (role)
	{
		case TitleRole:
		case UrlRole:
		case DescriptionRole:
		case IdentifierRole:
		case TypeRole:
			emit entryModified(entry);
			emit modelModified();

			break;
		default:
			break;
	}

	return true;
}

bool FeedsModel::hasFeed(const QUrl &url) const
{
	return (m_urls.contains(Utils::normalizeUrl(url)) || m_urls.contains(url));
}

}