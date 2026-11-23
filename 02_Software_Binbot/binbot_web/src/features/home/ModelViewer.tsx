import { useEffect, useRef, useState } from "react";
import * as THREE from "three";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";
import { DRACOLoader } from "three/addons/loaders/DRACOLoader.js";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";
import { decompressFrames, parseGIF, type ParsedFrame } from "gifuct-js";

type ModelViewerProps = {
  src?: string;
  gifSrc?: string;
  autoRotate?: boolean;
};

function ensureScreenUv(geometry: THREE.BufferGeometry) {
  if (geometry.getAttribute("uv")) return;
  const position = geometry.getAttribute("position");
  if (!position) return;
  geometry.computeBoundingBox();
  const bounds = geometry.boundingBox;
  if (!bounds) return;
  const width = Math.max(bounds.max.x - bounds.min.x, 0.0001);
  // SCREEN_1_3 is a long strip. Only its middle circular opening is exposed,
  // so map the square sticker to a diameter-sized section and leave the strip
  // on either side outside the [0, 1] range (the transparent edge is clamped).
  const diameter = width * 0.92;
  // The GLB node flips the strip's local longitudinal axis. In the rendered
  // device view the exposed circular area is therefore at the low-Z end.
  const centerZ = bounds.min.z + (bounds.max.z - bounds.min.z) * 0.22;
  const circleMinZ = centerZ - diameter * 0.5;
  const uv = new Float32Array(position.count * 2);
  for (let i = 0; i < position.count; i += 1) {
    const x = position.getX(i);
    const z = position.getZ(i);
    uv[i * 2] = (x - bounds.min.x) / width;
    uv[i * 2 + 1] = 1 - (z - circleMinZ) / diameter;
  }
  geometry.setAttribute("uv", new THREE.BufferAttribute(uv, 2));
}

function createAnimatedGifTexture(src: string) {
  const canvas = document.createElement("canvas");
  canvas.width = 240;
  canvas.height = 240;
  const context = canvas.getContext("2d");
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  // The generated screen UVs use the model's local top-to-bottom direction;
  // keep CanvasTexture's vertical flip so the GIF is upright on the device.
  texture.flipY = true;
  // Keep the sticker inside the circular screen instead of filling the whole
  // mesh. Clamp the transparent edge so the base material remains visible.
  texture.wrapS = THREE.ClampToEdgeWrapping;
  texture.wrapT = THREE.ClampToEdgeWrapping;
  let disposed = false;
  let frames: ParsedFrame[] = [];
  let frameIndex = 0;
  let previousFrame: (typeof frames)[number] | undefined;
  let nextFrameAt = 0;
  let loaded = false;
  let redraw = true;
  let staticImage: CanvasImageSource | null = null;
  let staticObjectUrl: string | null = null;
  const controller = new AbortController();

  const start = async () => {
    try {
      const response = await fetch(src, { signal: controller.signal });
      if (!response.ok) throw new Error("image could not be loaded");
      const blob = await response.blob();
      const bytes = await blob.arrayBuffer();
      if (disposed) return;
      const header = new TextDecoder().decode(bytes.slice(0, 6));
      if (header !== "GIF87a" && header !== "GIF89a") {
        if (typeof createImageBitmap === "function") {
          staticImage = await createImageBitmap(blob);
          if (disposed) {
            staticImage.close();
            staticImage = null;
            return;
          }
        } else {
          staticObjectUrl = URL.createObjectURL(blob);
          const image = new Image();
          image.src = staticObjectUrl;
          await new Promise<void>((resolve, reject) => {
            image.onload = () => resolve();
            image.onerror = () =>
              reject(new Error("image could not be loaded"));
          });
          if (disposed) return;
          staticImage = image;
        }
        if (context && staticImage) {
          canvas.width = 216;
          canvas.height = 216;
          context.clearRect(0, 0, canvas.width, canvas.height);
          const source = staticImage as CanvasImageSource & {
            width: number;
            height: number;
          };
          const scale = Math.min(216 / source.width, 216 / source.height);
          const width = source.width * scale;
          const height = source.height * scale;
          context.drawImage(
            source,
            (216 - width) / 2,
            (216 - height) / 2,
            width,
            height,
          );
          texture.needsUpdate = true;
          redraw = true;
          loaded = true;
        }
        return;
      }
      frames = decompressFrames(parseGIF(bytes), true);
      if (frames.length > 0 && context) {
        const width = Math.max(
          216,
          ...frames.map((item) => item.dims.left + item.dims.width),
        );
        const height = Math.max(
          216,
          ...frames.map((item) => item.dims.top + item.dims.height),
        );
        canvas.width = width;
        canvas.height = height;
      }
      loaded = Boolean(context && frames.length > 0);
    } catch {
      // The model remains usable if the optional animated sticker cannot load.
    }
  };
  if (src) void start();

  return {
    texture,
    update: (now: number) => {
      if (disposed) return false;
      const changed = redraw;
      redraw = false;
      if (!loaded || !context || staticImage || now < nextFrameAt)
        return changed;
      const frame = frames[frameIndex];
      if (previousFrame?.disposalType === 2) {
        context.clearRect(
          previousFrame.dims.left,
          previousFrame.dims.top,
          previousFrame.dims.width,
          previousFrame.dims.height,
        );
      }
      const patch = new ImageData(
        frame.patch as unknown as Uint8ClampedArray<ArrayBuffer>,
        frame.dims.width,
        frame.dims.height,
      );
      context.putImageData(patch, frame.dims.left, frame.dims.top);
      texture.needsUpdate = true;
      previousFrame = frame;
      frameIndex = (frameIndex + 1) % frames.length;
      nextFrameAt = now + Math.max(frame.delay || 100, 40);
      return true;
    },
    dispose: () => {
      disposed = true;
      controller.abort();
      frames = [];
      previousFrame = undefined;
      texture.dispose();
      if (staticImage && "close" in staticImage) staticImage.close();
      if (staticObjectUrl) URL.revokeObjectURL(staticObjectUrl);
    },
  };
}

export default function ModelViewer({
  src = "/1.glb",
  gifSrc = "/binbot-right-component.gif",
  autoRotate = true,
}: ModelViewerProps) {
  const hostRef = useRef<HTMLDivElement>(null);
  const [status, setStatus] = useState<"loading" | "ready" | "error">(
    "loading",
  );
  const [errorMessage, setErrorMessage] = useState("");
  const gifSourceRef = useRef(gifSrc);
  const gifTextureRef = useRef<ReturnType<
    typeof createAnimatedGifTexture
  > | null>(null);
  const screenMaterialsRef = useRef<THREE.MeshBasicMaterial[]>([]);
  gifSourceRef.current = gifSrc;

  useEffect(() => {
    if (!screenMaterialsRef.current.length) return;
    const previous = gifTextureRef.current;
    const next = createAnimatedGifTexture(gifSrc);
    gifTextureRef.current = next;
    screenMaterialsRef.current.forEach((material) => {
      material.map = next.texture;
      material.needsUpdate = true;
    });
    previous?.dispose();
  }, [gifSrc]);

  useEffect(() => {
    const host = hostRef.current;
    if (!host) return;
    setStatus("loading");
    setErrorMessage("");

    const scene = new THREE.Scene();
    const camera = new THREE.PerspectiveCamera(35, 1, 0.01, 1000);

    let renderer: THREE.WebGLRenderer;
    try {
      renderer = new THREE.WebGLRenderer({
        antialias: true,
        alpha: true,
        preserveDrawingBuffer: true,
      });
    } catch {
      setStatus("error");
      setErrorMessage("当前浏览器暂不支持 3D 预览");
      return;
    }
    renderer.setClearColor(0x000000, 0);
    renderer.domElement.setAttribute(
      "aria-label",
      "BinBot One 三维设备模型，可拖动旋转",
    );
    renderer.domElement.setAttribute("role", "img");
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    renderer.outputColorSpace = THREE.SRGBColorSpace;
    renderer.toneMapping = THREE.ACESFilmicToneMapping;
    renderer.toneMappingExposure = 1.05;
    host.appendChild(renderer.domElement);

    scene.add(new THREE.HemisphereLight("#ffffff", "#c8d1dc", 2.1));
    const keyLight = new THREE.DirectionalLight("#ffffff", 3.2);
    keyLight.position.set(3, 5, 4);
    scene.add(keyLight);
    const fillLight = new THREE.DirectionalLight("#b9d0f5", 1.4);
    fillLight.position.set(-4, 2, -2);
    scene.add(fillLight);

    const controls = new OrbitControls(camera, renderer.domElement);
    let sceneDirty = true;
    const invalidate = () => {
      sceneDirty = true;
    };
    controls.addEventListener("change", invalidate);
    controls.enableDamping = true;
    controls.dampingFactor = 0.08;
    controls.minDistance = 0.4;
    controls.maxDistance = 20;
    controls.autoRotate = autoRotate;
    controls.autoRotateSpeed = 1.1;
    controls.enableZoom = false;
    controls.enablePan = false;
    controls.minPolarAngle = Math.PI * 0.22;
    controls.maxPolarAngle = Math.PI * 0.65;
    controls.target.set(0, 0.6, 0);

    let model: THREE.Object3D | null = null;
    const gifOverlays: THREE.Mesh[] = [];
    let frame = 0;
    let disposed = false;

    const resize = () => {
      const width = Math.max(host.clientWidth, 1);
      const height = Math.max(host.clientHeight, 1);
      camera.aspect = width / height;
      camera.updateProjectionMatrix();
      renderer.setSize(width, height, false);
      invalidate();
    };
    const observer = new ResizeObserver(resize);
    observer.observe(host);
    resize();

    const loader = new GLTFLoader();
    const dracoLoader = new DRACOLoader();
    dracoLoader.setDecoderPath("/draco/");
    loader.setDRACOLoader(dracoLoader);
    loader.load(
      src,
      (gltf) => {
        if (disposed) return;
        model = gltf.scene;
        const box = new THREE.Box3().setFromObject(model);
        const center = box.getCenter(new THREE.Vector3());
        const size = box.getSize(new THREE.Vector3());
        const maxDimension = Math.max(size.x, size.y, size.z) || 1;
        // Keep the whole enclosure visible with a consistent breathing room.
        // The same scalar is applied to all axes, so the hardware proportions
        // from the GLB are never distorted.
        const scale = 1.35 / maxDimension;
        const scaledSize = size.clone().multiplyScalar(scale);
        model.scale.setScalar(scale);
        model.position.sub(center.multiplyScalar(scale));
        gifTextureRef.current = createAnimatedGifTexture(gifSourceRef.current);
        const targetMeshes: THREE.Mesh[] = [];
        model.traverse((object) => {
          if (
            !object.name.toUpperCase().includes("SCREEN_1_3") ||
            !(object as THREE.Mesh).isMesh
          )
            return;
          const mesh = object as THREE.Mesh;
          if (mesh.material) targetMeshes.push(mesh);
        });
        // Keep the GLB material untouched. The animated sticker is a transparent
        // duplicate drawn just above the original surface.
        targetMeshes.forEach((mesh) => {
          ensureScreenUv(mesh.geometry);
          const overlayMaterial = new THREE.MeshBasicMaterial({
            map: gifTextureRef.current!.texture,
            transparent: true,
            alphaTest: 0.01,
            depthTest: true,
            depthWrite: false,
            polygonOffset: true,
            polygonOffsetFactor: -1,
            polygonOffsetUnits: -1,
            toneMapped: false,
            side: THREE.DoubleSide,
          });
          screenMaterialsRef.current.push(overlayMaterial);
          const overlay = new THREE.Mesh(mesh.geometry, overlayMaterial);
          overlay.name = `${mesh.name}_animated_overlay`;
          overlay.position.copy(mesh.position);
          overlay.quaternion.copy(mesh.quaternion);
          overlay.scale.copy(mesh.scale);
          overlay.renderOrder = 2;
          mesh.parent?.add(overlay);
          gifOverlays.push(overlay);
        });
        scene.add(model);
        const radius = scaledSize.length() * 0.5;
        const halfFov = THREE.MathUtils.degToRad(camera.fov) * 0.5;
        const distance =
          (radius /
            Math.sin(
              Math.min(halfFov, Math.atan(Math.tan(halfFov) * camera.aspect)),
            )) *
          1.08;
        camera.position
          .set(0.7, 0.35, 1)
          .normalize()
          .multiplyScalar(distance * 0.9);
        camera.near = Math.max(distance / 100, 0.01);
        camera.far = distance * 100;
        camera.updateProjectionMatrix();
        controls.target.set(0, scaledSize.y * 0.08, 0);
        controls.update();
        setStatus("ready");
      },
      undefined,
      (error) => {
        if (!disposed) {
          const detail =
            error instanceof Error ? error.message : String(error || "");
          setErrorMessage(detail || "GLB 或 Draco 解码器无法解析");
          setStatus("error");
        }
      },
    );

    const animate = () => {
      if (disposed) return;
      frame = window.requestAnimationFrame(animate);
      controls.update();
      const textureChanged = gifTextureRef.current?.update(performance.now());
      if (sceneDirty || textureChanged) {
        renderer.render(scene, camera);
        sceneDirty = false;
      }
    };
    animate();

    return () => {
      disposed = true;
      window.cancelAnimationFrame(frame);
      observer.disconnect();
      controls.removeEventListener("change", invalidate);
      controls.dispose();
      if (model) {
        model.traverse((object) => {
          const mesh = object as THREE.Mesh;
          mesh.geometry?.dispose();
          if (Array.isArray(mesh.material))
            mesh.material.forEach((material) => material.dispose());
          else mesh.material?.dispose();
        });
      }
      gifOverlays.forEach((overlay) => {
        (overlay.material as THREE.Material).dispose();
      });
      screenMaterialsRef.current = [];
      gifTextureRef.current?.dispose();
      gifTextureRef.current = null;
      dracoLoader.dispose();
      renderer.dispose();
      renderer.domElement.remove();
    };
  }, [autoRotate, src]);

  return (
    <div className="model-viewer" ref={hostRef}>
      {status !== "ready" && (
        <div className="model-viewer-status">
          {status === "error" ? (
            <>
              <img
                className="model-illustration"
                src="/device-illustration.svg"
                alt="BinBot 设备示意图"
              />
              <small title={errorMessage}>设备示意 · 3D 预览暂不可用</small>
            </>
          ) : (
            <>
              <span className="model-loader" />
              <strong>正在加载设备模型</strong>
            </>
          )}
        </div>
      )}
    </div>
  );
}
