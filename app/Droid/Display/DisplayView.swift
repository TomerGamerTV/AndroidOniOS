import Metal
import QuartzCore
import UIKit

/// Shows the guest framebuffer (32bpp BGRA or RGBA, delivered by QEMU's main loop) with Metal and
/// forwards multitouch and hardware-keyboard input to the virtual device.
final class DisplayView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    private var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    private let device = MTLCreateSystemDefaultDevice()!
    private lazy var queue = device.makeCommandQueue()!
    private var pipeline: MTLRenderPipelineState!
    private var sampler: MTLSamplerState!

    // Double-buffered framebuffer copies (written on QEMU's thread, read on the main thread).
    private let lock = NSLock()
    private var buffers: [MTLBuffer] = []
    private var textures: [MTLTexture] = []
    private var front = -1
    private var writeIndex = 0
    private var fbWidth = 0, fbHeight = 0, fbStride = 0
    private var fbFormat = MTLPixelFormat.bgra8Unorm
    private var srcPixels: UnsafeMutableRawPointer?
    private var dirty = false
    private var displayLink: CADisplayLink?

    private(set) var guestSize = CGSize(width: 1080, height: 2400)
    var onFirstFrame: (() -> Void)?
    private var sawFirstFrame = false
    /// Quarter turns Android's content is rotated by (its Surface rotation: 1 = 90°, landscape with
    /// the device's top on the left). Android draws rotated content into its portrait framebuffer;
    /// the view turns it back upright and maps touches into the framebuffer's natural orientation.
    var rotation = 0 { didSet { dirty = true; setNeedsLayout() } }

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        backgroundColor = .black
        metalLayer.device = device
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
        metalLayer.contentsScale = UIScreen.main.nativeScale
        buildPipeline()
        displayLink = CADisplayLink(target: self, selector: #selector(tick))
        displayLink?.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: 120, preferred: 60)
        displayLink?.add(to: .main, forMode: .common)
    }

    required init?(coder: NSCoder) { fatalError() }

    override func layoutSubviews() {
        super.layoutSubviews()
        metalLayer.drawableSize = CGSize(width: bounds.width * metalLayer.contentsScale,
                                         height: bounds.height * metalLayer.contentsScale)
        dirty = true
    }

    private func buildPipeline() {
        let src = """
        #include <metal_stdlib>
        using namespace metal;
        struct V { float4 pos [[position]]; float2 uv; };
        vertex V vtx(uint vid [[vertex_id]], constant float4 &rect [[buffer(0)]], constant int &rot [[buffer(1)]]) {
            float2 p[4] = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };
            float2 q = p[vid];
            V o;
            o.pos = float4(rect.x + q.x * rect.z, rect.y - q.y * rect.w, 0, 1);
            // displayed point -> framebuffer point (the content is turned back by rot quarter turns)
            o.uv = rot == 1 ? float2(1 - q.y, q.x) : rot == 2 ? float2(1 - q.x, 1 - q.y) : rot == 3 ? float2(q.y, 1 - q.x) : q;
            return o;
        }
        fragment float4 frag(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
            return float4(t.sample(s, in.uv).rgb, 1.0);
        }
        """
        let lib = try! device.makeLibrary(source: src, options: nil)
        let d = MTLRenderPipelineDescriptor()
        d.vertexFunction = lib.makeFunction(name: "vtx")
        d.fragmentFunction = lib.makeFunction(name: "frag")
        d.colorAttachments[0].pixelFormat = .bgra8Unorm
        pipeline = try! device.makeRenderPipelineState(descriptor: d)
        let sd = MTLSamplerDescriptor()
        sd.minFilter = .linear
        sd.magFilter = .linear
        sampler = device.makeSamplerState(descriptor: sd)
    }

    // MARK: - QEMU callbacks (QEMU main-loop thread)

    /// Fills in the display callbacks; the bridge registers them after qemu_init.
    func attach(to qemu: inout DroidQEMU) {
        qemu.display_opaque = Unmanaged.passUnretained(self).toOpaque()
        qemu.display_surface = { opaque, pixels, w, h, stride, bgra in
            let view = Unmanaged<DisplayView>.fromOpaque(opaque!).takeUnretainedValue()
            view.surfaceChanged(pixels: pixels, width: Int(w), height: Int(h), stride: Int(stride),
                                format: bgra ? .bgra8Unorm : .rgba8Unorm)
        }
        qemu.display_update = { opaque, x, y, w, h in
            let view = Unmanaged<DisplayView>.fromOpaque(opaque!).takeUnretainedValue()
            view.surfaceUpdated()
        }
    }

    private func surfaceChanged(pixels: UnsafeMutableRawPointer?, width: Int, height: Int, stride: Int,
                                format: MTLPixelFormat) {
        lock.lock()
        defer { lock.unlock() }
        srcPixels = pixels
        guard pixels != nil, width > 0, height > 0 else { front = -1; return }
        if width != fbWidth || height != fbHeight || stride != fbStride || format != fbFormat || buffers.isEmpty {
            fbWidth = width; fbHeight = height; fbStride = stride; fbFormat = format
            let alignedStride = (width * 4 + 255) & ~255
            buffers = (0..<2).map { _ in device.makeBuffer(length: alignedStride * height, options: .storageModeShared)! }
            let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: width, height: height, mipmapped: false)
            td.usage = .shaderRead
            td.storageMode = .shared
            textures = buffers.map { $0.makeTexture(descriptor: td, offset: 0, bytesPerRow: alignedStride)! }
            front = -1
            DispatchQueue.main.async { self.guestSize = CGSize(width: width, height: height); self.setNeedsLayout() }
        }
        copyFrameLocked()
    }

    private func surfaceUpdated() {
        lock.lock()
        copyFrameLocked()
        lock.unlock()
    }

    private func copyFrameLocked() {
        guard let src = srcPixels, !buffers.isEmpty else { return }
        let dst = buffers[writeIndex].contents()
        let dstStride = (fbWidth * 4 + 255) & ~255
        let rowBytes = fbWidth * 4
        if dstStride == fbStride {
            memcpy(dst, src, fbStride * fbHeight)
        } else {
            for y in 0..<fbHeight {
                memcpy(dst + y * dstStride, src + y * fbStride, rowBytes)
            }
        }
        front = writeIndex
        writeIndex ^= 1
        dirty = true
        if !sawFirstFrame {
            sawFirstFrame = true
            DispatchQueue.main.async { self.onFirstFrame?() }
        }
    }

    // MARK: - Rendering (main thread)

    /// Guest framebuffer placement inside the view (aspect fit), in points.
    var contentRect: CGRect {
        let b = bounds
        guard guestSize.width > 0, guestSize.height > 0, b.width > 0 else { return b }
        let shown = rotation % 2 == 1 ? CGSize(width: guestSize.height, height: guestSize.width) : guestSize
        let scale = min(b.width / shown.width, b.height / shown.height)
        let w = shown.width * scale, h = shown.height * scale
        return CGRect(x: (b.width - w) / 2, y: (b.height - h) / 2, width: w, height: h)
    }

    @objc private func tick() {
        lock.lock()
        let idx = front
        let needs = dirty
        dirty = false
        let tex = idx >= 0 && idx < textures.count ? textures[idx] : nil
        lock.unlock()
        guard needs, let tex, let drawable = metalLayer.nextDrawable() else { return }

        let rp = MTLRenderPassDescriptor()
        rp.colorAttachments[0].texture = drawable.texture
        rp.colorAttachments[0].loadAction = .clear
        rp.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        rp.colorAttachments[0].storeAction = .store
        let cb = queue.makeCommandBuffer()!
        let enc = cb.makeRenderCommandEncoder(descriptor: rp)!
        let r = contentRect, b = bounds
        var rect = SIMD4<Float>(Float(r.minX / b.width * 2 - 1), Float(1 - r.minY / b.height * 2),
                                Float(r.width / b.width * 2), Float(r.height / b.height * 2))
        enc.setRenderPipelineState(pipeline)
        enc.setVertexBytes(&rect, length: MemoryLayout<SIMD4<Float>>.size, index: 0)
        var rot = Int32(rotation)
        enc.setVertexBytes(&rot, length: MemoryLayout<Int32>.size, index: 1)
        enc.setFragmentTexture(tex, index: 0)
        enc.setFragmentSamplerState(sampler, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        enc.endEncoding()
        cb.present(drawable)
        cb.commit()
    }

    // MARK: - Touch

    private var slots: [ObjectIdentifier: Int] = [:]

    private func guestPoint(_ t: UITouch) -> (Int, Int) {
        let r = contentRect
        let p = t.location(in: self)
        let dx = (p.x - r.minX) / r.width, dy = (p.y - r.minY) / r.height
        let (u, v) = rotation == 1 ? (1 - dy, dx) : rotation == 2 ? (1 - dx, 1 - dy) : rotation == 3 ? (dy, 1 - dx) : (dx, dy)
        let x = u * guestSize.width
        let y = v * guestSize.height
        return (Int(max(0, min(guestSize.width - 1, x))), Int(max(0, min(guestSize.height - 1, y))))
    }

    private func send(_ touches: Set<UITouch>, phase: Int32) {
        let vm = VirtualMachine.shared
        for t in touches {
            let id = ObjectIdentifier(t)
            var slot = slots[id]
            if phase == 0 {
                let used = Set(slots.values)
                guard let free = (0..<10).first(where: { !used.contains($0) }) else { continue }
                slots[id] = free
                slot = free
            }
            guard let s = slot else { continue }
            let (x, y) = guestPoint(t)
            vm.touch(slot: s, phase: phase, x: x, y: y, width: Int(guestSize.width), height: Int(guestSize.height))
            if phase == 2 { slots[id] = nil }
        }
    }

    // One log line per finger stroke (first 100 per launch): where it went down and up, how long it
    // took, how many moves were sent and whether iOS cancelled it. Taps that "press but don't open"
    // show whether the lift reached the VM.
    private struct Stroke { var start: (Int, Int); var began: Date; var moves = 0 }
    private var strokes: [ObjectIdentifier: Stroke] = [:]
    private var loggedStrokes = 0
    private func note(_ touches: Set<UITouch>, phase: Int32, cancelled: Bool = false) {
        guard loggedStrokes < 100 else { return }
        for t in touches {
            let id = ObjectIdentifier(t)
            if phase == 0 { strokes[id] = Stroke(start: guestPoint(t), began: Date()); continue }
            if phase == 1 { strokes[id]?.moves += 1; continue }
            guard let st = strokes.removeValue(forKey: id) else { continue }
            loggedStrokes += 1
            let end = guestPoint(t)
            Log.info(String(format: "touch: %d,%d -> %d,%d in %.0f ms, %d moves%@ (rotation %d, VM %@)", st.start.0, st.start.1, end.0, end.1,
                            Date().timeIntervalSince(st.began) * 1000, st.moves, cancelled ? ", cancelled" : "", rotation,
                            VirtualMachine.shared.started ? "running" : "not started"))
        }
    }
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) { note(touches, phase: 0); send(touches, phase: 0) }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) { note(touches, phase: 1); send(touches, phase: 1) }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { note(touches, phase: 2); send(touches, phase: 2) }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { note(touches, phase: 2, cancelled: true); send(touches, phase: 2) }

    // MARK: - Hardware keyboard

    override var canBecomeFirstResponder: Bool { true }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var handled = false
        for p in presses {
            if let k = p.key, let q = KeyMap.qcode(for: k.keyCode) {
                VirtualMachine.shared.key(q, down: true)
                handled = true
            }
        }
        if !handled { super.pressesBegan(presses, with: event) }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var handled = false
        for p in presses {
            if let k = p.key, let q = KeyMap.qcode(for: k.keyCode) {
                VirtualMachine.shared.key(q, down: false)
                handled = true
            }
        }
        if !handled { super.pressesEnded(presses, with: event) }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        pressesEnded(presses, with: event)
    }
}
