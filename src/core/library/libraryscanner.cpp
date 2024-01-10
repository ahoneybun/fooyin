﻿/*
 * Fooyin
 * Copyright 2022-2023, Luke Taylor <LukeT1@proton.me>
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

#include "libraryscanner.h"

#include "database/database.h"
#include "database/trackdatabase.h"
#include "library/libraryinfo.h"
#include "tagging/tagreader.h"

#include <core/track.h>
#include <utils/fileutils.h>

#include <QDir>

#include <ranges>

constexpr auto BatchSize = 250;

namespace {
Fooyin::Track matchMissingTrack(const Fooyin::TrackFieldMap& missingFiles, const Fooyin::TrackFieldMap& missingHashes,
                                Fooyin::Track& track)
{
    const QString filename = track.filename();
    const QString hash     = track.generateHash();

    if(missingFiles.contains(filename) && missingFiles.at(filename).duration() == track.duration()) {
        return missingFiles.at(filename);
    }

    if(missingHashes.contains(hash) && missingHashes.at(hash).duration() == track.duration()) {
        return missingHashes.at(hash);
    }

    return {};
};
} // namespace

namespace Fooyin {
struct LibraryScanner::Private
{
    LibraryScanner* self;

    LibraryInfo library;
    Database* database;
    TrackDatabase trackDatabase;

    TagReader tagReader;

    int tracksProcessed{0};
    double totalTracks{0};
    int currentProgress{-1};

    Private(LibraryScanner* self, Database* database)
        : self{self}
        , database{database}
        , trackDatabase{database->connectionName()}
    { }

    void reportProgress()
    {
        const int progress = static_cast<int>((tracksProcessed / totalTracks) * 100);
        if(currentProgress != progress) {
            currentProgress = progress;
            QMetaObject::invokeMethod(self, "progressChanged", Q_ARG(int, currentProgress));
        }
    }

    void storeTracks(TrackList& tracks)
    {
        if(!self->mayRun()) {
            return;
        }

        trackDatabase.storeTracks(tracks);
    }

    bool getAndSaveAllTracks(const TrackList& tracks)
    {
        const QDir dir{library.path};

        TrackList tracksToStore;
        TrackList tracksToUpdate;

        TrackFieldMap trackPaths;
        TrackFieldMap missingFiles;
        TrackFieldMap missingHashes;

        for(const Track& track : tracks) {
            trackPaths.emplace(track.filepath(), track);

            if(!QFileInfo::exists(track.filepath())) {
                missingFiles.emplace(track.filename(), track);
                missingHashes.emplace(track.hash(), track);
            }
        }

        const QStringList files = Utils::File::getFilesInDir(dir, Track::supportedFileExtensions());

        tracksProcessed = 0;
        totalTracks     = static_cast<double>(files.size());
        currentProgress = -1;

        for(const auto& filepath : files) {
            if(!self->mayRun()) {
                return false;
            }

            ++tracksProcessed;

            const QFileInfo info{filepath};
            const QDateTime lastModifiedTime{info.lastModified()};
            uint64_t lastModified{0};

            if(lastModifiedTime.isValid()) {
                lastModified = static_cast<uint64_t>(lastModifiedTime.toMSecsSinceEpoch());
            }

            auto setTrackProps = [this, &filepath, &dir](Track& track) {
                track.setFilePath(filepath);
                track.setLibraryId(library.id);
                track.setRelativePath(dir.relativeFilePath(filepath));
                track.setEnabled(true);
            };

            if(trackPaths.contains(filepath)) {
                const Track& libraryTrack = trackPaths.at(filepath);

                if(libraryTrack.libraryId() != library.id || libraryTrack.modifiedTime() != lastModified) {
                    Track changedTrack{libraryTrack};
                    if(tagReader.readMetaData(changedTrack)) {
                        changedTrack.generateHash();
                        setTrackProps(changedTrack);

                        tracksToUpdate.push_back(changedTrack);
                        missingHashes.erase(changedTrack.hash());
                    }
                }
            }
            else {
                Track track{filepath};

                if(tagReader.readMetaData(track)) {
                    Track refoundTrack = matchMissingTrack(missingFiles, missingHashes, track);

                    if(refoundTrack.isValid()) {
                        missingHashes.erase(refoundTrack.hash());
                        missingFiles.erase(refoundTrack.filename());

                        setTrackProps(refoundTrack);
                        tracksToUpdate.push_back(refoundTrack);
                    }
                    else {
                        tracksToStore.push_back(track);
                    }

                    if(tracksToStore.size() >= BatchSize) {
                        storeTracks(tracksToStore);
                        QMetaObject::invokeMethod(self, "scanUpdate",
                                                  Q_ARG(const ScanResult&, {.addedTracks = tracksToStore}));
                        tracksToStore.clear();
                    }
                }
            }

            reportProgress();
        }

        for(auto& track : missingHashes | std::views::values) {
            track.setLibraryId(-1);
            track.setEnabled(false);
            tracksToUpdate.push_back(track);
        }

        storeTracks(tracksToStore);
        storeTracks(tracksToUpdate);

        if(!tracksToStore.empty() || !tracksToUpdate.empty()) {
            QMetaObject::invokeMethod(self, "scanUpdate",
                                      Q_ARG(const ScanResult&, (ScanResult{tracksToStore, tracksToUpdate})));
        }

        return true;
    }

    void changeLibraryStatus(LibraryInfo::Status status)
    {
        library.status = status;
        QMetaObject::invokeMethod(self, "statusChanged", Q_ARG(const Fooyin::LibraryInfo&, library));
    }
};

LibraryScanner::LibraryScanner(Database* database, QObject* parent)
    : Worker{parent}
    , p{std::make_unique<Private>(this, database)}
{ }

LibraryScanner::~LibraryScanner() = default;

void LibraryScanner::closeThread()
{
    stopThread();
    p->database->closeDatabase();
}

void LibraryScanner::stopThread()
{
    emit progressChanged(100);
    setState(Idle);
}

void LibraryScanner::scanLibrary(const LibraryInfo& library, const TrackList& tracks)
{
    setState(Running);

    p->library = library;

    p->changeLibraryStatus(LibraryInfo::Status::Scanning);

    if(!QFileInfo::exists(p->library.path)) {
        // Root dir doesn't exist so leave to user to delete
        return;
    }

    p->getAndSaveAllTracks(tracks);

    if(state() == Paused) {
        p->changeLibraryStatus(LibraryInfo::Status::Pending);
    }
    else {
        p->changeLibraryStatus(LibraryInfo::Status::Idle);
        setState(Idle);
        emit finished();
    }
}

void LibraryScanner::scanTracks(const TrackList& libraryTracks, const TrackList& tracks)
{
    setState(Running);

    TrackList tracksScanned;
    TrackList tracksToStore;

    TrackFieldMap trackMap;
    std::ranges::transform(libraryTracks, std::inserter(trackMap, trackMap.end()),
                           [](const Track& track) { return std::make_pair(track.filepath(), track); });

    p->tracksProcessed = 0;
    p->totalTracks     = static_cast<double>(tracks.size());
    p->currentProgress = -1;

    const auto handleFinished = [this]() {
        if(state() != Paused) {
            setState(Idle);
            emit finished();
        }
    };

    for(const Track& pendingTrack : tracks) {
        if(!mayRun()) {
            handleFinished();
            return;
        }

        Track track{pendingTrack};

        ++p->tracksProcessed;

        if(trackMap.contains(track.filepath())) {
            tracksScanned.push_back(trackMap.at(track.filepath()));
        }
        else if(p->tagReader.readMetaData(track)) {
            track.generateHash();
            tracksToStore.push_back(track);
        }

        p->reportProgress();
    }

    p->storeTracks(tracksToStore);

    std::ranges::copy(tracksToStore, std::back_inserter(tracksScanned));

    emit scannedTracks(tracksScanned);

    handleFinished();
}
} // namespace Fooyin

#include "moc_libraryscanner.cpp"
