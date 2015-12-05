#include <QStandardPaths>
#include <QDir>
#include <QSqlError>

#include <QLoggingCategory>

#include "TimeLogHistoryWorker.h"

Q_LOGGING_CATEGORY(HISTORY_WORKER_CATEGORY, "TimeLogHistoryWorker", QtInfoMsg)

const int maxUndoSize(10);

const QString selectFields("SELECT uuid, start, category, comment, duration,"
                           " ifnull((SELECT start FROM timelog WHERE start < result.start ORDER BY start DESC LIMIT 1), 0)"
                           " FROM timelog AS result");

TimeLogHistoryWorker::TimeLogHistoryWorker(QObject *parent) :
    QObject(parent),
    m_isInitialized(false),
    m_size(0)
{

}

TimeLogHistoryWorker::~TimeLogHistoryWorker()
{
    QSqlDatabase::database(m_connectionName).close();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool TimeLogHistoryWorker::init(const QString &dataPath)
{
    QString path(QString("%1/timelog")
                 .arg(!dataPath.isEmpty() ? dataPath
                                          : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)));
    QDir().mkpath(path);
    m_connectionName = QString("timelog_%1").arg(qHash(dataPath));
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    db.setDatabaseName(QString("%1/%2.sqlite").arg(path).arg("db"));

    if (!db.open()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to open db:" << db.lastError().text();
        return false;
    }

    if (!setupTable()) {
        return false;
    }

    if (!setupTriggers()) {
        return false;
    }

    if (!updateSize()) {
        return false;
    }

    if (!updateCategories()) {
        return false;
    }

    m_isInitialized = true;

    return true;
}

qlonglong TimeLogHistoryWorker::size() const
{
    return m_size;
}

QSet<QString> TimeLogHistoryWorker::categories() const
{
    return m_categories;
}

void TimeLogHistoryWorker::insert(const TimeLogEntry &data)
{
    Q_ASSERT(m_isInitialized);

    Undo undo;
    undo.type = Undo::Insert;
    undo.data.append(data);
    pushUndo(undo);

    insertEntry(data);
}

void TimeLogHistoryWorker::import(const QVector<TimeLogEntry> &data)
{
    Q_ASSERT(m_isInitialized);

    if (insertData(data)) {
        emit dataImported(data);
    } else {
        processFail();
    }

    return;
}

void TimeLogHistoryWorker::remove(const TimeLogEntry &data)
{
    Q_ASSERT(m_isInitialized);

    TimeLogEntry entry = getEntry(data.uuid);
    Undo undo;
    undo.type = Undo::Remove;
    undo.data.append(entry);
    pushUndo(undo);

    removeEntry(data);
}

void TimeLogHistoryWorker::edit(const TimeLogEntry &data, TimeLogHistory::Fields fields)
{
    Q_ASSERT(m_isInitialized);

    TimeLogEntry entry = getEntry(data.uuid);
    Undo undo;
    undo.type = Undo::Edit;
    undo.data.append(entry);
    undo.fields.append(fields);
    pushUndo(undo);

    editEntry(data, fields);
}

void TimeLogHistoryWorker::editCategory(QString oldName, QString newName)
{
    Q_ASSERT(m_isInitialized);

    if (newName.isEmpty()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Empty category name";
        emit error("Empty category name");
        return;
    } else if (oldName == newName) {
        qCWarning(HISTORY_WORKER_CATEGORY) << "Same category name:" << newName;
        return;
    }

    QVector<TimeLogEntry> entries = getEntries(oldName);
    Undo undo;
    undo.type = Undo::EditCategory;
    undo.data.swap(entries);
    undo.fields.insert(0, undo.data.size(), TimeLogHistory::Category);
    pushUndo(undo);

    if (editCategoryData(oldName, newName)) {
        emit dataOutdated();    // TODO: more precise update signal
    } else {
        processFail();
    }
}

void TimeLogHistoryWorker::sync(const QVector<TimeLogSyncData> &updatedData, const QVector<TimeLogSyncData> &removedData)
{
    Q_ASSERT(m_isInitialized);

    QVector<TimeLogSyncData> removedNew;
    QVector<TimeLogSyncData> removedOld;
    QVector<TimeLogSyncData> insertedNew;
    QVector<TimeLogSyncData> insertedOld;
    QVector<TimeLogSyncData> updatedNew;
    QVector<TimeLogSyncData> updatedOld;

    foreach (const TimeLogSyncData &entry, removedData) {
        QVector<TimeLogSyncData> affected = getSyncAffected(entry.uuid);
        if (!affected.isEmpty() && affected.constFirst().mTime >= entry.mTime) {
            continue;
        }

        removedNew.append(entry);
        removedOld.append(affected.isEmpty() ? TimeLogSyncData() : affected.constFirst());
    }

    foreach (const TimeLogSyncData &entry, updatedData) {
        QVector<TimeLogSyncData> affected = getSyncAffected(entry.uuid);
        if (!affected.isEmpty() && affected.constFirst().mTime >= entry.mTime) {
            continue;
        }

        if (affected.isEmpty() || !affected.constFirst().isValid()) {
            insertedNew.append(entry);
            insertedOld.append(affected.isEmpty() ? TimeLogSyncData() : affected.constFirst());
        } else {
            updatedNew.append(entry);
            updatedOld.append(affected.constFirst());
        }
    }

    emit syncStatsAvailable(removedOld, removedNew, insertedOld, insertedNew, updatedOld, updatedNew);

    QVector<TimeLogSyncData> removedMerged(removedNew.size());
    for (int i = 0; i < removedMerged.size(); i++) {
        removedMerged[i] = removedOld.at(i);
        removedMerged[i].uuid = removedNew.at(i).uuid;
        removedMerged[i].mTime = removedNew.at(i).mTime;
    }

    if (syncData(removedMerged, insertedNew, updatedNew, updatedOld)) {
        emit dataSynced(updatedData, removedData);
    }
}

void TimeLogHistoryWorker::undo()
{
    if (!m_undoStack.size()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Empty undo stack";
        return;
    }

    Undo undo = m_undoStack.pop();
    switch (undo.type) {
    case Undo::Insert:
        removeEntry(undo.data.constFirst());
        break;
    case Undo::Remove:
        insertEntry(undo.data.constFirst());
        break;
    case Undo::Edit:
        editEntry(undo.data.constFirst(), undo.fields.constFirst());
        break;
    case Undo::EditCategory:
        editEntries(undo.data, undo.fields);
        break;
    }

    emit undoCountChanged(m_undoStack.size());
}

void TimeLogHistoryWorker::getHistoryBetween(qlonglong id, const QDateTime &begin, const QDateTime &end, const QString &category) const
{
    Q_ASSERT(m_isInitialized);

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString = QString("%1 WHERE (start BETWEEN ? AND ?) %2 ORDER BY start ASC")
                                  .arg(selectFields)
                                  .arg(category.isEmpty() ? "" : "AND category=?");
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        emit historyRequestCompleted(QVector<TimeLogEntry>(), id);
        return;
    }
    query.addBindValue(begin.toTime_t());
    query.addBindValue(end.toTime_t());
    if (!category.isEmpty()) {
        query.addBindValue(category);
    }

    emit historyRequestCompleted(getHistory(query), id);
}

void TimeLogHistoryWorker::getHistoryAfter(qlonglong id, const uint limit, const QDateTime &from) const
{
    Q_ASSERT(m_isInitialized);

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString = QString("%1 WHERE start > ? ORDER BY start ASC LIMIT ?").arg(selectFields);
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        emit historyRequestCompleted(QVector<TimeLogEntry>(), id);
        return;
    }
    query.addBindValue(from.toTime_t());
    query.addBindValue(limit);

    emit historyRequestCompleted(getHistory(query), id);
}

void TimeLogHistoryWorker::getHistoryBefore(qlonglong id, const uint limit, const QDateTime &until) const
{
    Q_ASSERT(m_isInitialized);

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString = QString("%1 WHERE start < ? ORDER BY start DESC LIMIT ?").arg(selectFields);
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        emit historyRequestCompleted(QVector<TimeLogEntry>(), id);
        return;
    }
    query.addBindValue(until.toTime_t());
    query.addBindValue(limit);

    QVector<TimeLogEntry> result = getHistory(query);
    if (!result.isEmpty()) {
        std::reverse(result.begin(), result.end());
    }
    emit historyRequestCompleted(result, id);
}

void TimeLogHistoryWorker::getStats(const QDateTime &begin, const QDateTime &end, const QString &category, const QString &separator) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString = QString("WITH result AS ( "
                                  "    SELECT rtrim(substr(category, 1, ifnull(%1, length(category)))) as category, CASE "
                                  "        WHEN duration!=-1 THEN duration "
                                  "        ELSE (SELECT strftime('%s','now')) - (SELECT start FROM timelog ORDER BY start DESC LIMIT 1) "
                                  "        END AS duration "
                                  "    FROM timelog "
                                  "    WHERE (start BETWEEN :sBegin AND :sEnd) %2 "
                                  ") "
                                  "SELECT category, SUM(duration) FROM result "
                                  " GROUP BY category "
                                  " ORDER BY category ASC")
            .arg(category.isEmpty() ? "nullif(instr(category, :separator) - 1, -1)"
                                    : "nullif(instr(substr(category, nullif(instr(substr(category, length(:category) + 1), :separator), 0) + 1 + length(:category)), :separator), 0) + length(:category)")
            .arg(category.isEmpty() ? "" : "AND category LIKE :category || '%'");
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        return;
    }
    query.bindValue(":sBegin", begin.toTime_t());
    query.bindValue(":sEnd", end.toTime_t());
    query.bindValue(":separator", separator);
    if (!category.isEmpty()) {
        query.bindValue(":category", category);
    }

    emit statsDataAvailable(getStats(query), end);
}

void TimeLogHistoryWorker::getSyncData(const QDateTime &mBegin, const QDateTime &mEnd) const
{
    Q_ASSERT(m_isInitialized);

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString("WITH result AS ( "
                        "    SELECT uuid, start, category, comment, mtime FROM timelog "
                        "    WHERE (mtime > :mBegin AND mtime <= :mEnd) "
                        "UNION ALL "
                        "    SELECT uuid, NULL, NULL, NULL, mtime FROM removed "
                        "    WHERE (mtime > :mBegin AND mtime <= :mEnd) "
                        ") "
                        "SELECT * FROM result ORDER BY mtime ASC");
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        return;
    }
    query.bindValue(":mBegin", mBegin.toMSecsSinceEpoch());
    query.bindValue(":mEnd", mEnd.toMSecsSinceEpoch());

    emit syncDataAvailable(getSyncData(query), mEnd);
}

bool TimeLogHistoryWorker::setupTable()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString("CREATE TABLE IF NOT EXISTS timelog"
                        " (uuid BLOB UNIQUE, start INTEGER PRIMARY KEY, category TEXT, comment TEXT,"
                        " duration INTEGER, mtime INTEGER);");
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        return false;
    }

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery();
        return false;
    }

    queryString = "CREATE TABLE IF NOT EXISTS removed (uuid BLOB UNIQUE, mtime INTEGER);";
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        return false;
    }

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery();
        return false;
    }

    return true;
}

bool TimeLogHistoryWorker::setupTriggers()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString;

    queryString = "CREATE TRIGGER IF NOT EXISTS check_insert_timelog BEFORE INSERT ON timelog "
                  "BEGIN "
                  "    SELECT mtime, "
                  "        CASE WHEN NEW.mtime < mtime "
                  "            THEN RAISE(IGNORE) "
                  "        END "
                  "    FROM removed WHERE uuid=NEW.uuid; "
                  "END;";
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        return false;
    }

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery();
        return false;
    }

    queryString = "CREATE TRIGGER IF NOT EXISTS insert_timelog AFTER INSERT ON timelog "
                  "BEGIN "
                  "    UPDATE timelog SET duration=(NEW.start - start) "
                  "    WHERE start=( "
                  "        SELECT start FROM timelog WHERE start < NEW.start ORDER BY start DESC LIMIT 1 "
                  "    ); "
                  "    UPDATE timelog SET duration=IFNULL( "
                  "        ( SELECT start FROM timelog WHERE start > NEW.start ORDER BY start ASC LIMIT 1 ) - NEW.start, "
                  "        -1 "
                  "    ) WHERE start=NEW.start; "
                  "    DELETE FROM removed WHERE uuid=NEW.uuid; "
                  "END;";
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        return false;
    }

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery();
        return false;
    }

    queryString = "CREATE TRIGGER IF NOT EXISTS delete_timelog AFTER DELETE ON timelog "
                  "BEGIN "
                  "    UPDATE timelog SET duration=IFNULL( "
                  "        ( SELECT start FROM timelog WHERE start > OLD.start ORDER BY start ASC LIMIT 1 ) - start, "
                  "        -1 "
                  "    ) WHERE start=( "
                  "        SELECT start FROM timelog WHERE start < OLD.start ORDER BY start DESC LIMIT 1 "
                  "    ); "
                  "END;";
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        return false;
    }

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery();
        return false;
    }

    queryString = "CREATE TRIGGER IF NOT EXISTS check_update_timelog BEFORE UPDATE ON timelog "
                  "BEGIN "
                  "    SELECT "
                  "        CASE WHEN NEW.mtime < OLD.mtime "
                  "            THEN RAISE(IGNORE) "
                  "        END; "
                  "END;";
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        return false;
    }

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery();
        return false;
    }

    queryString = "CREATE TRIGGER IF NOT EXISTS update_timelog AFTER UPDATE OF start ON timelog "
                  "BEGIN "
                  "    UPDATE timelog SET duration=(NEW.start - start) "
                  "    WHERE start=( "
                  "        SELECT start FROM timelog WHERE start < NEW.start ORDER BY start DESC LIMIT 1 "
                  "    ); "
                  "    UPDATE timelog SET duration=IFNULL( "
                  "        ( SELECT start FROM timelog WHERE start > OLD.start ORDER BY start ASC LIMIT 1 ) - start,"
                  "        -1"
                  "    ) WHERE start=NULLIF( "  // If previous item not changed, do not update it's duration twice
                  "        ( SELECT start FROM timelog WHERE start < OLD.start ORDER BY start DESC LIMIT 1 ), "
                  "        ( SELECT start FROM timelog WHERE start < NEW.start ORDER BY start DESC LIMIT 1 ) "
                  "    ); "
                  "    UPDATE timelog SET duration=IFNULL( "
                  "        ( SELECT start FROM timelog WHERE start > NEW.start ORDER BY start ASC LIMIT 1 ) - NEW.start, "
                  "        -1 "
                  "    ) WHERE start=NEW.start; "
                  "END;";
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        return false;
    }

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery();
        return false;
    }

    queryString = "CREATE TRIGGER IF NOT EXISTS check_insert_removed BEFORE INSERT ON removed "
                  "BEGIN "
                  "    SELECT mtime, "
                  "        CASE WHEN NEW.mtime < mtime "
                  "            THEN RAISE(IGNORE) "
                  "        END "
                  "    FROM removed WHERE uuid=NEW.uuid; "
                  "END;";
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        return false;
    }

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery();
        return false;
    }

    queryString = "CREATE TRIGGER IF NOT EXISTS insert_removed AFTER INSERT ON removed "
                  "BEGIN "
                  "    DELETE FROM timelog WHERE uuid=NEW.uuid; "
                  "END;";
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        return false;
    }

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery();
        return false;
    }

    return true;
}

void TimeLogHistoryWorker::setSize(qlonglong size)
{
    if (m_size == size) {
        return;
    }

    m_size = size;

    emit sizeChanged(m_size);
}

void TimeLogHistoryWorker::removeFromCategories(QString category)
{
    if (!m_categories.contains(category)) {
        return;
    }

    m_categories.remove(category);

    emit categoriesChanged(m_categories);
}

void TimeLogHistoryWorker::addToCategories(QString category)
{
    if (m_categories.contains(category)) {
        return;
    }

    m_categories.insert(category);

    emit categoriesChanged(m_categories);
}

void TimeLogHistoryWorker::processFail()
{
    m_undoStack.clear();
    emit undoCountChanged(0);

    emit dataOutdated();
}

void TimeLogHistoryWorker::insertEntry(const TimeLogEntry &data)
{
    if (insertData(data)) {
        emit dataInserted(data);
        notifyInsertUpdates(data);
    } else {
        processFail();
    }
}

void TimeLogHistoryWorker::removeEntry(const TimeLogEntry &data)
{
    if (removeData(data)) {
        emit dataRemoved(data);
        notifyRemoveUpdates(data);
    } else {
        processFail();
    }
}

bool TimeLogHistoryWorker::editEntry(const TimeLogEntry &data, TimeLogHistory::Fields fields)
{
    QDateTime oldStart;
    if (fields == TimeLogHistory::NoFields) {
        qCWarning(HISTORY_WORKER_CATEGORY) << "No fields specified";
        return false;
    } else if (fields & TimeLogHistory::StartTime) {
        TimeLogEntry oldData = getEntry(data.uuid);
        if (!oldData.isValid()) {
            qCCritical(HISTORY_WORKER_CATEGORY) << "Item to update not found:\n"
                                                << data.startTime << data.category << data.uuid;
            processFail();
            return false;
        }
        oldStart = oldData.startTime;
    }

    if (editData(data, fields)) {
        notifyEditUpdates(data, fields, oldStart);
    }  else {
        processFail();
        return false;
    }

    return true;
}

void TimeLogHistoryWorker::editEntries(const QVector<TimeLogEntry> &data, const QVector<TimeLogHistory::Fields> &fields)
{
    for (int i = 0; i < data.size(); i++) {
        if (!editEntry(data.at(i), fields.at(i))) {
            break;
        }
    }
}

bool TimeLogHistoryWorker::insertData(const QVector<TimeLogEntry> &data)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.transaction()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to start transaction:" << db.lastError().text();
        emit error(db.lastError().text());
        return false;
    }

    foreach (const TimeLogEntry &entry, data) {
        if (!insertData(entry)) {
            if (!db.rollback()) {
                qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to rollback transaction:" << db.lastError().text();
                emit error(db.lastError().text());
            }

            return false;
        }
    }

    if (!db.commit()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to commit transaction:" << db.lastError().text();
        emit error(db.lastError().text());
        if (!db.rollback()) {
            qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to rollback transaction:" << db.lastError().text();
            emit error(db.lastError().text());
        }
        return false;
    }

    return true;
}

bool TimeLogHistoryWorker::insertData(const TimeLogSyncData &data)
{
    Q_ASSERT(data.isValid());

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString = QString("INSERT INTO timelog (uuid, start, category, comment, mtime)"
                                  " VALUES (?,?,?,?,?);");
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        return false;
    }
    query.addBindValue(data.uuid.toRfc4122());
    query.addBindValue(data.startTime.toTime_t());
    query.addBindValue(data.category);
    query.addBindValue(data.comment);
    query.addBindValue(data.mTime.isValid() ? data.mTime.toMSecsSinceEpoch()
                                            : QDateTime::currentMSecsSinceEpoch());

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery() << query.boundValues();
        emit error(query.lastError().text());
        return false;
    }

    setSize(m_size + query.numRowsAffected());
    addToCategories(data.category);

    return true;
}

bool TimeLogHistoryWorker::removeData(const TimeLogSyncData &data)
{
    Q_ASSERT(!data.uuid.isNull());

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString("INSERT OR REPLACE INTO removed (uuid, mtime) VALUES(?,?);");
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        return false;
    }
    query.addBindValue(data.uuid.toRfc4122());
    query.addBindValue(data.mTime.isValid() ? data.mTime.toMSecsSinceEpoch()
                                            : QDateTime::currentMSecsSinceEpoch());

    if (!query.exec()) {
        qCWarning(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                           << query.executedQuery() << query.boundValues();
        emit error(query.lastError().text());
        return false;
    }

    setSize(m_size - query.numRowsAffected());

    return true;
}

bool TimeLogHistoryWorker::editData(const TimeLogSyncData &data, TimeLogHistory::Fields fields)
{
    Q_ASSERT(data.isValid());
    Q_ASSERT(fields != TimeLogHistory::NoFields);

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QStringList fieldNames;
    if (fields & TimeLogHistory::StartTime) {
        fieldNames.append("start=?");
    }
    if (fields & TimeLogHistory::Category) {
        fieldNames.append("category=?");
    }
    if (fields & TimeLogHistory::Comment) {
        fieldNames.append("comment=?");
    }
    QString queryString = QString("UPDATE timelog SET %1, mtime=?"
                                  " WHERE uuid=?;").arg(fieldNames.join(", "));
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        return false;
    }
    if (fields & TimeLogHistory::StartTime) {
        query.addBindValue(data.startTime.toTime_t());
    }
    if (fields & TimeLogHistory::Category) {
        query.addBindValue(data.category);
    }
    if (fields & TimeLogHistory::Comment) {
        query.addBindValue(data.comment);
    }
    query.addBindValue(data.mTime.isValid() ? data.mTime.toMSecsSinceEpoch()
                                            : QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(data.uuid.toRfc4122());

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery() << query.boundValues();
        emit error(query.lastError().text());
        return false;
    }

    if (fields & TimeLogHistory::Category) {
        addToCategories(data.category);
    }

    return true;
}

bool TimeLogHistoryWorker::editCategoryData(QString oldName, QString newName)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString("SELECT count(*) FROM timelog WHERE category=?");
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        return false;
    }
    query.addBindValue(oldName);

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery() << query.boundValues();
        emit error(query.lastError().text());
        return false;
    }

    query.next();
    bool hasOldCategoryItems = query.value(0).toULongLong() > 0;
    query.finish();

    if (!hasOldCategoryItems) {
        removeFromCategories(oldName);
        return false;
    }

    queryString = QString("UPDATE timelog SET category=?, mtime=? WHERE category=?;");
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        return false;
    }
    query.addBindValue(newName);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(oldName);

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery() << query.boundValues();
        emit error(query.lastError().text());
        return false;
    }

    if (!updateCategories()) {
        return false;
    }

    return true;
}

bool TimeLogHistoryWorker::syncData(const QVector<TimeLogSyncData> &removed, const QVector<TimeLogSyncData> &inserted, const QVector<TimeLogSyncData> &updatedNew, const QVector<TimeLogSyncData> &updatedOld)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.transaction()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to start transaction:" << db.lastError().text();
        emit error(db.lastError().text());
        return false;
    }

    foreach (const TimeLogSyncData &entry, removed) {
        if (!removeData(entry)) {
            if (!db.rollback()) {
                qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to rollback transaction:" << db.lastError().text();
                emit error(db.lastError().text());
            }

            return false;
        }
    }

    foreach (const TimeLogSyncData &entry, inserted) {
        if (!insertData(entry)) {
            if (!db.rollback()) {
                qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to rollback transaction:" << db.lastError().text();
                emit error(db.lastError().text());
            }

            return false;
        }
    }

    foreach (const TimeLogSyncData &entry, updatedNew) {
        if (!editData(entry, TimeLogHistory::AllFieldsMask)) {
            if (!db.rollback()) {
                qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to rollback transaction:" << db.lastError().text();
                emit error(db.lastError().text());
            }

            return false;
        }
    }

    if (!db.commit()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to commit transaction:" << db.lastError().text();
        emit error(db.lastError().text());
        if (!db.rollback()) {
            qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to rollback transaction:" << db.lastError().text();
            emit error(db.lastError().text());
        }
        return false;
    }

    foreach (const TimeLogEntry &entry, removed) {
        if (entry.isValid()) {
            emit dataRemoved(entry);
        }
    }
    foreach (const TimeLogEntry &entry, removed) {
        if (entry.isValid()) {
            notifyRemoveUpdates(entry);
        }
    }
    foreach (const TimeLogEntry &entry, inserted) {
        emit dataInserted(entry);
    }
    foreach (const TimeLogEntry &entry, inserted) {
        notifyInsertUpdates(entry);
    }
    for (int i = 0; i < updatedNew.size(); i++) {
        const TimeLogSyncData &newField = updatedNew.at(i);
        const TimeLogSyncData &oldField = updatedOld.at(i);
        TimeLogHistory::Fields fields(TimeLogHistory::NoFields);
        if (newField.startTime != oldField.startTime) {
            fields |= TimeLogHistory::StartTime;
        }
        if (newField.category != oldField.category) {
            fields |= TimeLogHistory::Category;
        }
        if (newField.comment != oldField.comment) {
            fields |= TimeLogHistory::Comment;
        }
        notifyEditUpdates(updatedNew.at(i), fields, oldField.startTime);
    }

    return true;
}

QVector<TimeLogEntry> TimeLogHistoryWorker::getHistory(QSqlQuery &query) const
{
    QVector<TimeLogEntry> result;

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery() << query.boundValues();
        emit error(query.lastError().text());
        return result;
    }

    while (query.next()) {
        TimeLogEntry data;
        data.uuid = QUuid::fromRfc4122(query.value(0).toByteArray());
        data.startTime = QDateTime::fromTime_t(query.value(1).toUInt());
        data.category = query.value(2).toString();
        data.comment = query.value(3).toString();
        data.durationTime = query.value(4).toInt();
        data.precedingStart = QDateTime::fromTime_t(query.value(5).toUInt());

        result.append(data);
    }

    query.finish();

    return result;
}

QVector<TimeLogStats> TimeLogHistoryWorker::getStats(QSqlQuery &query) const
{
    QVector<TimeLogStats> result;

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery() << query.boundValues();
        emit error(query.lastError().text());
        return result;
    }

    while (query.next()) {
        TimeLogStats data;
        data.category = query.value(0).toString();
        data.durationTime = query.value(1).toInt();

        result.append(data);
    }

    query.finish();

    return result;
}

QVector<TimeLogSyncData> TimeLogHistoryWorker::getSyncData(QSqlQuery &query) const
{
    QVector<TimeLogSyncData> result;

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery() << query.boundValues();
        emit error(query.lastError().text());
        return result;
    }

    while (query.next()) {
        TimeLogSyncData data;
        data.uuid = QUuid::fromRfc4122(query.value(0).toByteArray());
        data.startTime = QDateTime::fromTime_t(query.value(1).toUInt());
        data.category = query.value(2).toString();
        data.comment = query.value(3).toString();
        data.mTime = QDateTime::fromMSecsSinceEpoch(query.value(4).toLongLong());

        result.append(data);
    }

    query.finish();

    return result;
}

TimeLogEntry TimeLogHistoryWorker::getEntry(const QUuid &uuid) const
{
    TimeLogEntry entry;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString = QString("%1 WHERE uuid=?").arg(selectFields);
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        return entry;
    }
    query.addBindValue(uuid.toRfc4122());

    QVector<TimeLogEntry> data = getHistory(query);
    if (!data.empty()) {
        entry = data.first();
    }
    return entry;
}

QVector<TimeLogEntry> TimeLogHistoryWorker::getEntries(const QString &category) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString = QString("%1 WHERE category=?").arg(selectFields);
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        return QVector<TimeLogEntry>();
    }
    query.addBindValue(category);

    return getHistory(query);
}

QVector<TimeLogSyncData> TimeLogHistoryWorker::getSyncAffected(const QUuid &uuid) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString("WITH result AS ( "
                        "    SELECT uuid, start, category, comment, mtime FROM timelog "
                        "    WHERE uuid=:uuid "
                        "UNION ALL "
                        "    SELECT uuid, NULL, NULL, NULL, mtime FROM removed "
                        "    WHERE uuid=:uuid "
                        ") "
                        "SELECT * FROM result ORDER BY mtime DESC LIMIT 1");
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        return QVector<TimeLogSyncData>();
    }
    query.bindValue(":uuid", uuid.toRfc4122());

    return getSyncData(query);
}

void TimeLogHistoryWorker::notifyInsertUpdates(const TimeLogEntry &data) const
{
    QString queryString = QString("SELECT * FROM ( "
                                  "    %1 WHERE start <= :newStart ORDER BY start DESC LIMIT 2 "
                                  ") "
                                  "UNION "
                                  "SELECT * FROM ( "
                                  "    %1 WHERE start > :newStart ORDER BY start ASC LIMIT 1 "
                                  ")").arg(selectFields);
    QMap<QString, QDateTime> bindParameters;
    bindParameters[":newStart"] = data.startTime;
    notifyUpdates(queryString, bindParameters);
}

void TimeLogHistoryWorker::notifyInsertUpdates(const QVector<TimeLogEntry> &data) const
{
    foreach (const TimeLogEntry &entry, data) {
        notifyInsertUpdates(entry);    // TODO: optimize
    }
}

void TimeLogHistoryWorker::notifyRemoveUpdates(const TimeLogEntry &data) const
{
    QString queryString = QString("SELECT * FROM ( "
                                  "    %1 WHERE start < :oldStart ORDER BY start DESC LIMIT 1 "
                                  ") "
                                  "UNION "
                                  "SELECT * FROM ( "
                                  "    %1 WHERE start > :oldStart ORDER BY start ASC LIMIT 1 "
                                  ")").arg(selectFields);
    QMap<QString, QDateTime> bindParameters;
    bindParameters[":oldStart"] = data.startTime;
    notifyUpdates(queryString, bindParameters);
}

void TimeLogHistoryWorker::notifyEditUpdates(const TimeLogEntry &data, TimeLogHistory::Fields fields, QDateTime oldStart) const
{
    QString queryString;
    QMap<QString, QDateTime> bindParameters;

    if (fields & TimeLogHistory::StartTime) {
        queryString = QString("SELECT * FROM ( "
                              "    %1 WHERE start <= :newStart ORDER BY start DESC LIMIT 2 "
                              ") "
                              "UNION "
                              "SELECT * FROM ( "
                              "    %1 WHERE start > :newStart ORDER BY start ASC LIMIT 1 "
                              ") "
                              "UNION "
                              "SELECT * FROM ( "
                              "    %1 WHERE start < :oldStart ORDER BY start DESC LIMIT 1 "
                              ") "
                              "UNION "
                              "SELECT * FROM ( "
                              "    %1 WHERE start > :oldStart ORDER BY start ASC LIMIT 1 "
                              ")").arg(selectFields);
        bindParameters[":newStart"] = data.startTime;
        bindParameters[":oldStart"] = oldStart;
        fields |= TimeLogHistory::DurationTime | TimeLogHistory::PrecedingStart;
    } else {
        queryString = QString("%1 WHERE start=:start").arg(selectFields);
        bindParameters[":start"] = data.startTime;
    }

    notifyUpdates(queryString, bindParameters, fields);
}

void TimeLogHistoryWorker::notifyUpdates(const QString &queryString, const QMap<QString, QDateTime> &values, TimeLogHistory::Fields fields) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    if (!query.prepare(QString("%1 ORDER BY start ASC").arg(queryString))) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        return;
    }
    for (QMap<QString, QDateTime>::const_iterator it = values.cbegin(); it != values.cend(); it++) {
        query.bindValue(it.key(), it.value().toTime_t());
    }

    QVector<TimeLogEntry> updatedData = getHistory(query);
    QVector<TimeLogHistory::Fields> updatedFields;

    if (!updatedData.isEmpty()) {
        qCDebug(HISTORY_WORKER_CATEGORY) << "Updated items count:" << updatedData.size();
        updatedFields.insert(0, updatedData.size(), fields);
        emit dataUpdated(updatedData, updatedFields);
    }
}

bool TimeLogHistoryWorker::updateSize()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString = QString("SELECT count(*) FROM timelog");
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        return false;
    }

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery();
        emit error(query.lastError().text());
        return false;
    }

    query.next();
    setSize(query.value(0).toULongLong());
    query.finish();

    return true;
}

bool TimeLogHistoryWorker::updateCategories(const QDateTime &begin, const QDateTime &end)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(db);
    QString queryString("SELECT DISTINCT category FROM timelog"
                        " WHERE start BETWEEN ? AND ?");
    if (!query.prepare(queryString)) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to prepare query:" << query.lastError().text()
                                            << query.lastQuery();
        emit error(query.lastError().text());
        return false;
    }
    query.addBindValue(begin.toTime_t());
    query.addBindValue(end.toTime_t());

    QSet<QString> result;

    if (!query.exec()) {
        qCCritical(HISTORY_WORKER_CATEGORY) << "Fail to execute query:" << query.lastError().text()
                                            << query.executedQuery() << query.boundValues();
        emit error(query.lastError().text());
        return false;
    }

    while (query.next()) {
        result.insert(query.value(0).toString());
    }

    query.finish();

    m_categories.swap(result);
    emit categoriesChanged(m_categories);

    return true;
}

void TimeLogHistoryWorker::pushUndo(const TimeLogHistoryWorker::Undo undo)
{
    m_undoStack.push(undo);

    if (m_undoStack.size() > maxUndoSize) {
        m_undoStack.takeFirst();
    } else {
        emit undoCountChanged(m_undoStack.size());
    }
}