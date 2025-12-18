<?php
namespace ProfiWeb\Cloud\Tests\Compat;

class NamedValue
{
    public $label;
    public $payload;

    public function __construct(string $label, $payload)
    {
        $this->label = $label;
        $this->payload = $payload;
    }

    public function __toString(): string
    {
        return $this->label;
    }
}

interface CompatTestInterface
{
    public function joinParts($first, $second, $third = '');
    public function wrapComplex($label, $payload);
    public function reflectValue($value);
    public function multiArgSpread($first, $second, $third);
    public function largeData($length);
}

class CompatTestHandler implements CompatTestInterface
{
    public function joinParts($first, $second, $third = '')
    {
        $segments = [
            trim((string) $first),
            trim((string) $second),
            trim((string) $third),
        ];

        return implode('|', $segments);
    }

    public function wrapComplex($label, $payload)
    {
        return [
            'label' => (string) $label,
            'payload' => $payload,
            'type' => is_object($payload) ? get_class($payload) : gettype($payload),
        ];
    }

    public function reflectValue($value)
    {
        return $value;
    }

    public function multiArgSpread($first, $second, $third)
    {
        return [$first, $second, $third];
    }

    public function largeData($length)
    {
        $size = max(0, (int) $length);

        return str_repeat('L', $size);
    }
}
