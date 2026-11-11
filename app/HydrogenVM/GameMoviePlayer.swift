import AVFoundation
import SwiftUI
import UIKit

/// A video file the game asked to play.
struct GameMovieRequest: Identifiable, Equatable {
    let id: UInt64
    let url: URL
}

/// Plays a game's video file with the system player, fitted to the screen. `finished` is
/// called once: when the video reaches its end, or when it cannot be played.
struct GameMoviePlayer: UIViewRepresentable {
    let request: GameMovieRequest
    let volume: Float
    let finished: () -> Void

    func makeCoordinator() -> Coordinator { Coordinator(finished: finished) }

    func makeUIView(context: Context) -> PlayerView {
        let view = PlayerView()
        view.backgroundColor = .black
        view.isUserInteractionEnabled = false
        let player = AVPlayer(url: request.url)
        player.volume = volume
        player.actionAtItemEnd = .pause
        view.playerLayer.player = player
        view.playerLayer.videoGravity = .resizeAspect
        context.coordinator.watch(player)
        player.play()
        return view
    }

    func updateUIView(_ view: PlayerView, context: Context) {
        view.playerLayer.player?.volume = volume
    }

    static func dismantleUIView(_ view: PlayerView, coordinator: Coordinator) {
        coordinator.stop()
        view.playerLayer.player?.pause()
        view.playerLayer.player = nil
    }

    final class PlayerView: UIView {
        override class var layerClass: AnyClass { AVPlayerLayer.self }
        var playerLayer: AVPlayerLayer { layer as! AVPlayerLayer }
    }

    final class Coordinator: NSObject {
        private let finished: () -> Void
        private var done = false
        private var observers: [NSObjectProtocol] = []
        private var status: NSKeyValueObservation?
        private var watchdog: Timer?

        init(finished: @escaping () -> Void) { self.finished = finished }

        func watch(_ player: AVPlayer) {
            guard let item = player.currentItem else { finish(); return }
            let center = NotificationCenter.default
            for name in [AVPlayerItem.didPlayToEndTimeNotification, AVPlayerItem.failedToPlayToEndTimeNotification] {
                observers.append(center.addObserver(forName: name, object: item, queue: .main) { [weak self] _ in
                    self?.finish()
                })
            }
            // A file the system player cannot decode never reaches its end: say so at once.
            status = item.observe(\.status, options: [.new]) { [weak self] item, _ in
                guard item.status == .failed else { return }
                DispatchQueue.main.async { self?.finish() }
            }
            // And a player that never starts must not hold the game forever.
            watchdog = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self, weak player] _ in
                guard let self, let player else { return }
                if player.currentItem?.status != .readyToPlay { self.finish() }
            }
        }

        func stop() {
            done = true
            observers.forEach { NotificationCenter.default.removeObserver($0) }
            observers.removeAll()
            status = nil
            watchdog?.invalidate()
            watchdog = nil
        }

        private func finish() {
            guard !done else { return }
            stop()
            finished()
        }
    }
}
