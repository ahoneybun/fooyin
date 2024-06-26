/*
 * Fooyin
 * Copyright © 2022, Luke Taylor <LukeT1@proton.me>
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

#include <core/player/playercontroller.h>

#include <core/player/playbackqueue.h>

#include <core/coresettings.h>
#include <core/track.h>
#include <utils/settings/settingsmanager.h>

namespace Fooyin {
struct PlayerController::Private
{
    PlayerController* self;

    SettingsManager* settings;

    PlaylistTrack currentTrack;
    uint64_t totalDuration{0};
    PlayState playStatus{PlayState::Stopped};
    Playlist::PlayModes playMode;
    uint64_t position{0};
    bool counted{false};
    bool isQueueTrack{false};

    PlaybackQueue queue;

    Private(PlayerController* self_, SettingsManager* settings_)
        : self{self_}
        , settings{settings_}
        , playMode{static_cast<Playlist::PlayModes>(settings->value<Settings::Core::PlayMode>())}
    { }
};

PlayerController::PlayerController(SettingsManager* settings, QObject* parent)
    : QObject{parent}
    , p{std::make_unique<Private>(this, settings)}
{
    settings->subscribe<Settings::Core::PlayMode>(this, [this]() {
        const auto mode = static_cast<Playlist::PlayModes>(p->settings->value<Settings::Core::PlayMode>());
        if(std::exchange(p->playMode, mode) != mode) {
            emit playModeChanged(mode);
        }
    });
}

PlayerController::~PlayerController() = default;

void PlayerController::reset()
{
    p->playStatus = PlayState::Stopped;
    p->position   = 0;
}

void PlayerController::play()
{
    if(!p->currentTrack.isValid() && !p->queue.empty()) {
        changeCurrentTrack(p->queue.nextTrack());
        emit tracksDequeued({p->currentTrack});
    }

    if(p->currentTrack.isValid() && p->playStatus != PlayState::Playing) {
        p->playStatus = PlayState::Playing;
        emit playStateChanged(p->playStatus);
    }
}

void PlayerController::playPause()
{
    switch(p->playStatus) {
        case(PlayState::Playing):
            pause();
            break;
        case(PlayState::Paused):
        case(PlayState::Stopped):
            play();
            break;
        default:
            break;
    }
}

void PlayerController::pause()
{
    if(std::exchange(p->playStatus, PlayState::Paused) != p->playStatus) {
        emit playStateChanged(p->playStatus);
    }
}

void PlayerController::previous()
{
    emit previousTrack();
}

void PlayerController::next()
{
    if(p->queue.empty()) {
        p->isQueueTrack = false;
        emit nextTrack();
    }
    else {
        p->currentTrack = {};
        p->isQueueTrack = true;
        play();
    }
}

void PlayerController::stop()
{
    if(std::exchange(p->playStatus, PlayState::Stopped) != p->playStatus) {
        reset();
        emit playStateChanged(p->playStatus);
    }
}

void PlayerController::setCurrentPosition(uint64_t ms)
{
    p->position = ms;
    // TODO: Only increment playCount based on total time listened excluding seeking.
    if(!p->counted && ms >= p->totalDuration / 2) {
        p->counted = true;
        if(p->currentTrack.isValid()) {
            emit trackPlayed(p->currentTrack.track);
        }
    }
    emit positionChanged(ms);
}

void PlayerController::changeCurrentTrack(const Track& track)
{
    changeCurrentTrack(PlaylistTrack{track, {}});
}

void PlayerController::changeCurrentTrack(const PlaylistTrack& track)
{
    p->currentTrack  = track;
    p->totalDuration = p->currentTrack.track.duration();
    p->position      = 0;
    p->counted       = false;

    emit currentTrackChanged(p->currentTrack.track);
    emit playlistTrackChanged(p->currentTrack);
}

void PlayerController::updateCurrentTrackPlaylist(const Id& playlistId)
{
    if(std::exchange(p->currentTrack.playlistId, playlistId) != playlistId) {
        emit playlistTrackChanged(p->currentTrack);
    }
}

void PlayerController::updateCurrentTrackIndex(int index)
{
    if(std::exchange(p->currentTrack.indexInPlaylist, index) != index) {
        emit playlistTrackChanged(p->currentTrack);
    }
}

PlaybackQueue PlayerController::playbackQueue() const
{
    return p->queue;
}

void PlayerController::setPlayMode(Playlist::PlayModes mode)
{
    p->settings->set<Settings::Core::PlayMode>(static_cast<int>(mode));
}

void PlayerController::seek(uint64_t ms)
{
    if(p->totalDuration < 100) {
        return;
    }

    if(ms >= p->totalDuration - 100) {
        next();
        return;
    }

    if(std::exchange(p->position, ms) != ms) {
        emit positionMoved(ms);
    }
}

void PlayerController::seekForward(uint64_t delta)
{
    seek(p->position + delta);
}

void PlayerController::seekBackward(uint64_t delta)
{
    if(delta > p->position) {
        seek(0);
    }
    else {
        seek(p->position - delta);
    }
}

PlayState PlayerController::playState() const
{
    return p->playStatus;
}

Playlist::PlayModes PlayerController::playMode() const
{
    return p->playMode;
}

uint64_t PlayerController::currentPosition() const
{
    return p->position;
}

Track PlayerController::currentTrack() const
{
    return p->currentTrack.track;
}

int PlayerController::currentTrackId() const
{
    return p->currentTrack.isValid() ? p->currentTrack.track.id() : -1;
}

bool PlayerController::currentIsQueueTrack() const
{
    return p->isQueueTrack;
}

PlaylistTrack PlayerController::currentPlaylistTrack() const
{
    return p->currentTrack;
}

void PlayerController::queueTrack(const Track& track)
{
    queueTrack(PlaylistTrack{track, {}});
}

void PlayerController::queueTrack(const PlaylistTrack& track)
{
    queueTracks({track});
}

void PlayerController::queueTracks(const TrackList& tracks)
{
    QueueTracks tracksToQueue;
    for(const Track& track : tracks) {
        tracksToQueue.emplace_back(track);
    }

    queueTracks(tracksToQueue);
}

void PlayerController::queueTracks(const QueueTracks& tracks)
{
    p->queue.addTracks(tracks);
    emit tracksQueued(tracks);
}

void PlayerController::dequeueTrack(const Track& track)
{
    dequeueTrack(PlaylistTrack{track, {}});
}

void PlayerController::dequeueTrack(const PlaylistTrack& track)
{
    dequeueTracks({track});
}

void PlayerController::dequeueTracks(const TrackList& tracks)
{
    QueueTracks tracksToDequeue;
    for(const Track& track : tracks) {
        tracksToDequeue.emplace_back(track);
    }

    dequeueTracks(tracksToDequeue);
}

void PlayerController::dequeueTracks(const QueueTracks& tracks)
{
    const auto removedTracks = p->queue.removeTracks(tracks);
    if(!removedTracks.empty()) {
        emit tracksDequeued(removedTracks);
    }
}

void PlayerController::replaceTracks(const QueueTracks& tracks)
{
    QueueTracks removed;

    const auto currentTracks = p->queue.tracks();
    const std::set<PlaylistTrack> newTracks{tracks.cbegin(), tracks.cend()};

    std::ranges::copy_if(currentTracks, std::back_inserter(removed),
                         [&newTracks](const PlaylistTrack& oldTrack) { return !newTracks.contains(oldTrack); });

    p->queue.clear();
    p->queue.addTracks(tracks);

    emit trackQueueChanged(removed, tracks);
}

void PlayerController::clearPlaylistQueue(const Id& playlistId)
{
    const auto removedTracks = p->queue.removePlaylistTracks(playlistId);
    if(!removedTracks.empty()) {
        emit tracksDequeued(removedTracks);
    }
}
} // namespace Fooyin

#include "core/player/moc_playercontroller.cpp"
