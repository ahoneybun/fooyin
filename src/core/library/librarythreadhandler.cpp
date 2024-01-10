/*
 * Fooyin
 * Copyright 2022-2024, Luke Taylor <LukeT1@proton.me>
 *
 * Fooyin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Fooyin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Fooyin.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "librarythreadhandler.h"

#include "internalcoresettings.h"
#include "library/libraryinfo.h"
#include "library/librarymanager.h"
#include "libraryscanner.h"
#include "trackdatabasemanager.h"

#include <core/library/musiclibrary.h>
#include <utils/settings/settingsmanager.h>

#include <QThread>

#include <deque>
#include <ranges>

namespace {
int nextRequestId()
{
    static int requestId{0};
    return requestId++;
}
} // namespace

namespace Fooyin {
struct LibraryScanRequest : ScanRequest
{
    LibraryInfo library;
    QString dir;
    TrackList tracks;
};

struct LibraryThreadHandler::Private
{
    LibraryThreadHandler* self;

    Database* database;
    MusicLibrary* library;
    SettingsManager* settings;

    QThread* thread;
    LibraryScanner scanner;
    TrackDatabaseManager trackDatabaseManager;

    std::deque<std::unique_ptr<LibraryScanRequest>> scanRequests;
    int currentRequestId{-1};

    Private(LibraryThreadHandler* self, Database* database, MusicLibrary* library, SettingsManager* settings)
        : self{self}
        , database{database}
        , library{library}
        , settings{settings}
        , thread{new QThread(self)}
        , scanner{database, settings}
        , trackDatabaseManager{database}
    {
        scanner.moveToThread(thread);
        trackDatabaseManager.moveToThread(thread);

        QObject::connect(library, &MusicLibrary::tracksScanned, self, [this]() {
            if(!scanRequests.empty()) {
                execNextRequest();
            }
        });

        thread->start();
    }

    void scanLibrary(const LibraryScanRequest& request)
    {
        currentRequestId = request.id;
        QMetaObject::invokeMethod(&scanner, "scanLibrary", Q_ARG(const LibraryInfo&, request.library),
                                  Q_ARG(const TrackList&, library->tracks()));
    }

    void scanTracks(const LibraryScanRequest& request)
    {
        currentRequestId = request.id;
        QMetaObject::invokeMethod(&scanner, "scanTracks", Q_ARG(const TrackList&, library->tracks()),
                                  Q_ARG(const TrackList&, request.tracks));
    }

    void scanDirectory(const LibraryScanRequest& request)
    {
        currentRequestId = request.id;
        QMetaObject::invokeMethod(&scanner, "scanLibraryDirectory", Q_ARG(const LibraryInfo&, request.library),
                                  Q_ARG(const QString&, request.dir), Q_ARG(const TrackList&, library->tracks()));
    }

    ScanRequest* addLibraryScanRequest(const LibraryInfo& library)
    {
        auto* request = scanRequests
                            .emplace_back(std::make_unique<LibraryScanRequest>(
                                ScanRequest{ScanRequest::Library, nextRequestId(), nullptr}, library, "", TrackList{}))
                            .get();
        request->cancel = [this, request]() {
            cancelScanRequest(request->id);
        };

        if(scanRequests.size() == 1) {
            scanLibrary(*request);
        }
        return request;
    }

    ScanRequest* addTracksScanRequest(const TrackList& tracks)
    {
        if(!scanRequests.empty()) {
            scanner.pauseThread();
        }

        LibraryScanRequest* request
            = scanRequests
                  .emplace_front(std::make_unique<LibraryScanRequest>(
                      ScanRequest{ScanRequest::Tracks, nextRequestId(), nullptr}, LibraryInfo{}, "", tracks))
                  .get();
        request->cancel = [this, request]() {
            cancelScanRequest(request->id);
        };

        scanTracks(*request);

        return request;
    }

    ScanRequest* addDirectoryScanRequest(const LibraryInfo& library, const QString& dir)
    {
        auto* request = scanRequests
                            .emplace_back(std::make_unique<LibraryScanRequest>(
                                ScanRequest{ScanRequest::Library, nextRequestId(), nullptr}, library, dir, TrackList{}))
                            .get();
        request->cancel = [this, request]() {
            cancelScanRequest(request->id);
        };

        if(scanRequests.size() == 1) {
            scanDirectory(*request);
        }
        return request;
    }

    void execNextRequest()
    {
        const LibraryScanRequest& request = *scanRequests.front();
        switch(request.type) {
            case(ScanRequest::Tracks):
                scanTracks(request);
                break;
            case(ScanRequest::Library):
                if(request.dir.isEmpty()) {
                    scanLibrary(request);
                }
                else {
                    scanDirectory(request);
                }
                break;
        }
    }

    void finishScanRequest()
    {
        const bool scanType = scanRequests.front()->type;

        scanRequests.pop_front();
        currentRequestId = -1;

        if(scanRequests.empty() || scanType == ScanRequest::Tracks) {
            return;
        }

        execNextRequest();
    }

    void cancelScanRequest(int id)
    {
        if(currentRequestId == id) {
            scanner.stopThread();
        }
        else {
            std::erase_if(scanRequests, [id](const auto& request) { return request->id == id; });
        }
    }
};

LibraryThreadHandler::LibraryThreadHandler(Database* database, MusicLibrary* library, LibraryManager* libraryManager,
                                           SettingsManager* settings, QObject* parent)
    : QObject{parent}
    , p{std::make_unique<Private>(this, database, library, settings)}
{
    QObject::connect(&p->trackDatabaseManager, &TrackDatabaseManager::gotTracks, this,
                     &LibraryThreadHandler::gotTracks);
    QObject::connect(&p->trackDatabaseManager, &TrackDatabaseManager::updatedTracks, this,
                     &LibraryThreadHandler::tracksUpdated);
    QObject::connect(&p->scanner, &Worker::finished, this, [this]() { p->finishScanRequest(); });
    QObject::connect(&p->scanner, &LibraryScanner::progressChanged, this,
                     [this](int percent) { emit progressChanged(p->currentRequestId, percent); });
    QObject::connect(&p->scanner, &LibraryScanner::statusChanged, this, &LibraryThreadHandler::statusChanged);
    QObject::connect(&p->scanner, &LibraryScanner::scanUpdate, this, &LibraryThreadHandler::scanUpdate);
    QObject::connect(&p->scanner, &LibraryScanner::scannedTracks, this, &LibraryThreadHandler::scannedTracks);
    QObject::connect(
        &p->scanner, &LibraryScanner::directoryChanged, this,
        [this](const LibraryInfo& library, const QString& dir) { p->addDirectoryScanRequest(library, dir); });

    auto setupWatchers = [this, libraryManager](bool enabled) {
        QMetaObject::invokeMethod(&p->scanner, "setupWatchers",
                                  Q_ARG(const LibraryInfoMap&, libraryManager->allLibraries()), enabled);
    };

    p->settings->subscribe<Settings::Core::Internal::MonitorLibraries>(
        this, [this, libraryManager, setupWatchers](bool enabled) {
            setupWatchers(enabled);

            if(enabled) {
                const LibraryInfoMap& libraries = libraryManager->allLibraries();
                for(const auto& library : libraries | std::views::values) {
                    p->addLibraryScanRequest(library);
                }
            }
        });

    setupWatchers(p->settings->value<Settings::Core::Internal::MonitorLibraries>());
}

LibraryThreadHandler::~LibraryThreadHandler()
{
    p->scanner.stopThread();
    p->thread->quit();
    p->thread->wait();
}

void LibraryThreadHandler::getAllTracks()
{
    QMetaObject::invokeMethod(&p->trackDatabaseManager, &TrackDatabaseManager::getAllTracks);
}

void LibraryThreadHandler::scanLibrary(const LibraryInfo& library)
{
    p->addLibraryScanRequest(library);
}

ScanRequest* LibraryThreadHandler::scanTracks(const TrackList& tracks)
{
    return p->addTracksScanRequest(tracks);
}

void LibraryThreadHandler::libraryRemoved(int id)
{
    if(p->scanRequests.empty()) {
        return;
    }

    const LibraryScanRequest& request = *p->scanRequests.front();

    if(request.type == ScanRequest::Library && request.library.id == id) {
        p->scanner.stopThread();
    }
    else {
        std::erase_if(p->scanRequests, [id](const auto& request) { return request->library.id == id; });
    }
}

void LibraryThreadHandler::saveUpdatedTracks(const TrackList& tracks)
{
    QMetaObject::invokeMethod(&p->trackDatabaseManager, "updateTracks", Q_ARG(const TrackList&, tracks));
}

void LibraryThreadHandler::saveUpdatedTrackStats(const Track& track)
{
    QMetaObject::invokeMethod(&p->trackDatabaseManager, "updateTrackStats", Q_ARG(const Track&, track));
}

void LibraryThreadHandler::cleanupTracks()
{
    QMetaObject::invokeMethod(&p->trackDatabaseManager, &TrackDatabaseManager::cleanupTracks);
}
} // namespace Fooyin

#include "moc_librarythreadhandler.cpp"
