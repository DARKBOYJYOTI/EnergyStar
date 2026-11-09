namespace EnergyStar
{
    public enum EnergyMode
    {
        Auto,
        AlwaysOn,
        Off
    }

    public sealed class EnergyStatus
    {
        public required EnergyMode Mode { get; init; }
        public required bool OnBattery { get; init; }
        public required bool IsEnabled { get; init; }
        public required int ManagedProcessCount { get; init; }
    }
}
