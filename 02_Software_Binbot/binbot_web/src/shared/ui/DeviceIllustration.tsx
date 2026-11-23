import { useId } from "react";

export default function DeviceIllustration({
  color = "lilac",
  asleep = false,
}: {
  color?: "lilac" | "mint" | "peach";
  asleep?: boolean;
}) {
  const id = useId().replaceAll(":", "");
  const tint =
    color === "mint" ? "#b5cbbd" : color === "peach" ? "#e0c5b6" : "#c7c5d8";
  return (
    <svg
      className="device-illustration"
      viewBox="0 0 300 260"
      fill="none"
      aria-hidden="true"
    >
      <defs>
        <linearGradient
          id={`${id}-body`}
          x1="75"
          y1="50"
          x2="218"
          y2="216"
          gradientUnits="userSpaceOnUse"
        >
          <stop stopColor="#fff" />
          <stop offset="1" stopColor={tint} />
        </linearGradient>
        <linearGradient
          id={`${id}-stand`}
          x1="121"
          y1="192"
          x2="167"
          y2="239"
          gradientUnits="userSpaceOnUse"
        >
          <stop stopColor="#f8f8f7" />
          <stop offset="1" stopColor={tint} />
        </linearGradient>
        <linearGradient
          id={`${id}-screen`}
          x1="86"
          y1="85"
          x2="215"
          y2="176"
          gradientUnits="userSpaceOnUse"
        >
          <stop stopColor="#36343d" />
          <stop offset="1" stopColor="#17171d" />
        </linearGradient>
        <radialGradient id={`${id}-shadow`}>
          <stop stopColor="#5f596a" stopOpacity=".19" />
          <stop offset="1" stopColor="#5f596a" stopOpacity="0" />
        </radialGradient>
      </defs>
      <ellipse cx="151" cy="232" rx="102" ry="17" fill={`url(#${id}-shadow)`} />
      <path
        d="M132 181h39l14 43c2 6-9 10-35 10s-37-4-35-10l17-43Z"
        fill={`url(#${id}-stand)`}
      />
      <path
        d="M143 55V36"
        stroke={tint}
        strokeWidth="8"
        strokeLinecap="round"
      />
      <circle
        cx="143"
        cy="31"
        r="8"
        fill="#eeefdc"
        stroke="#fff"
        strokeWidth="3"
      />
      <rect
        x="60"
        y="55"
        width="182"
        height="147"
        rx="46"
        fill={`url(#${id}-body)`}
      />
      <rect
        x="76"
        y="72"
        width="149"
        height="111"
        rx="33"
        fill={`url(#${id}-screen)`}
      />
      <path
        d="M84 95c3-9 10-15 21-16"
        stroke="#fff"
        strokeOpacity=".16"
        strokeWidth="3"
        strokeLinecap="round"
      />
      {asleep ? (
        <>
          <path
            d="m111 125 14 5 14-5m24 0 14 5 14-5"
            stroke="#d1e7bd"
            strokeWidth="5"
            strokeLinecap="round"
            strokeLinejoin="round"
          />
          <path
            d="M210 55h15l-14 16h15"
            stroke="#8e86a3"
            strokeWidth="3"
            strokeLinecap="round"
            strokeLinejoin="round"
          />
        </>
      ) : (
        <>
          <rect x="115" y="114" width="10" height="21" rx="5" fill="#d8eeab" />
          <rect x="176" y="114" width="10" height="21" rx="5" fill="#d8eeab" />
          <path
            d="M141 143q10 9 20 0"
            stroke="#d8eeab"
            strokeWidth="4"
            strokeLinecap="round"
          />
        </>
      )}
      <ellipse
        cx="101"
        cy="143"
        rx="8"
        ry="4"
        fill="#e8c3ba"
        fillOpacity=".6"
      />
      <ellipse
        cx="198"
        cy="143"
        rx="8"
        ry="4"
        fill="#e8c3ba"
        fillOpacity=".6"
      />
      <path
        d="M69 181c-16 9-24 4-25-9m188 4c15 10 24 4 26-8"
        stroke={tint}
        strokeWidth="10"
        strokeLinecap="round"
      />
      <circle cx="150" cy="192" r="3" fill="#8d99a0" />
    </svg>
  );
}
