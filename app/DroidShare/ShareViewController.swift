import UIKit
import UniformTypeIdentifiers

/// "Android" in the share sheet: drops shared files (APKs install automatically) into the
/// app group inbox, then opens the main app which installs/copies them over ADB.
final class ShareViewController: UIViewController {
    private let label = UILabel()

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemBackground
        label.text = "Sending to Android…"
        label.font = .preferredFont(forTextStyle: .headline)
        label.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(label)
        NSLayoutConstraint.activate([label.centerXAnchor.constraint(equalTo: view.centerXAnchor),
                                     label.centerYAnchor.constraint(equalTo: view.centerYAnchor)])
        Task { await collect() }
    }

    private var inbox: URL {
        let base = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: "group.com.tomergamertv.oni")!
        let u = base.appendingPathComponent("Inbox", isDirectory: true)
        try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }

    private func collect() async {
        let items = (extensionContext?.inputItems as? [NSExtensionItem]) ?? []
        var count = 0
        for item in items {
            for provider in item.attachments ?? [] {
                if await copy(provider) { count += 1 }
            }
        }
        label.text = count > 0 ? "Sent \(count) item\(count == 1 ? "" : "s") to Android" : "Nothing to send"
        openHostApp()
        try? await Task.sleep(nanoseconds: 600_000_000)
        extensionContext?.completeRequest(returningItems: nil)
    }

    private func copy(_ provider: NSItemProvider) async -> Bool {
        let types = [UTType("com.android.package-archive")?.identifier, UTType.data.identifier].compactMap { $0 }
        for t in types where provider.hasItemConformingToTypeIdentifier(t) {
            let ok: Bool = await withCheckedContinuation { cont in
                provider.loadFileRepresentation(forTypeIdentifier: t) { url, _ in
                    guard let url else { cont.resume(returning: false); return }
                    var name = provider.suggestedName ?? url.lastPathComponent
                    if (name as NSString).pathExtension.isEmpty { name += "." + url.pathExtension }
                    let dest = self.inbox.appendingPathComponent(name)
                    try? FileManager.default.removeItem(at: dest)
                    do { try FileManager.default.copyItem(at: url, to: dest); cont.resume(returning: true) }
                    catch { cont.resume(returning: false) }
                }
            }
            if ok { return true }
        }
        return false
    }

    /// Share extensions can't call UIApplication.open directly; walk the responder chain.
    private func openHostApp() {
        guard let url = URL(string: "androidonios://inbox") else { return }
        var r: UIResponder? = self
        while let cur = r {
            if let app = cur as? UIApplication {
                app.open(url, options: [:], completionHandler: nil)
                return
            }
            r = cur.next
        }
    }
}
