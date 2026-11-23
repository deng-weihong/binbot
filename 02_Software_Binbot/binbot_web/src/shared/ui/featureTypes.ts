import type { Dispatch, SetStateAction } from "react";
import type { Device, DeviceData } from "../model/data";

export type FeaturePageProps = {
  device: Device;
  data: DeviceData;
  setData: Dispatch<SetStateAction<DeviceData>>;
  notify: (message: string) => void;
};
